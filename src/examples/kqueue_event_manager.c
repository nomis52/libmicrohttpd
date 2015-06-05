/*
     This file is part of libmicrohttpd
     Copyright (C) 2007 Christian Grothoff (and other contributing authors)

     This library is free software; you can redistribute it and/or
     modify it under the terms of the GNU Lesser General Public
     License as published by the Free Software Foundation; either
     version 2.1 of the License, or (at your option) any later version.

     This library is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     Lesser General Public License for more details.

     You should have received a copy of the GNU Lesser General Public
     License along with this library; if not, write to the Free Software
     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
/**
 * @file kqueue_event_manager.c
 * @brief A basic kqueue / kevent event manager.
 * @author Simon Newton
 */

#include "event_manager.h"
#include <microhttpd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>

struct EventManager
{
  int kqueue_fd;
  struct MHD_Timeout *timeout_head;
  struct MHD_Timeout *timeout_tail;

  struct MHD_Watch *orphaned_watches;
  MHD_EventManager *interface;

  uint8_t stop : 1;

};

struct MHD_Watch
{
  int fd;
  MHD_WatchCallback callback;
  EventManager *em;
  void *mhd_data;

  uint8_t enable_read : 1;
  uint8_t enable_write : 1;
  uint8_t deleted : 1;

  MHD_Watch *next;
};

struct MHD_Timeout
{
  MHD_TimeoutCallback callback;
  EventManager *em;
  void *mhd_data;
  struct timeval trigger_time;

  /*
   * timeouts are stored in a doubly linked list. To avoid walking the entire
   * list each cycle, this should be changed to a priority queue.
   */
  struct MHD_Timeout *next;
  struct MHD_Timeout *previous;
};

static void
watch_update (MHD_Watch *watch,
              MHD_WatchEvent events)
{
  struct kevent change_set[2];
  int change_index = 0;

  int enable_read = events & MHD_WATCH_IN ? 1 : 0;
  if (enable_read != watch->enable_read)
    {
    if (enable_read)
      {
        EV_SET (&change_set[change_index], watch->fd, EVFILT_READ, EV_ADD, 0, 0, watch);
      }
      else
      {
        EV_SET (&change_set[change_index], watch->fd, EVFILT_READ, EV_DELETE, 0, 0, watch);
      }
    watch->enable_read = enable_read;
    change_index++;
  }

  int enable_write = events & MHD_WATCH_OUT ? 1: 0;
  if (enable_write != watch->enable_write)
    {
    if (enable_write)
      {
        EV_SET (&change_set[change_index], watch->fd, EVFILT_WRITE, EV_ADD, 0, 0, watch);
      }
      else
      {
        EV_SET (&change_set[change_index], watch->fd, EVFILT_WRITE, EV_DELETE, 0, 0, watch);
      }
    watch->enable_write = enable_write;
    change_index++;
  }

  int r = kevent (watch->em->kqueue_fd,
                  (struct kevent*) &change_set,
                  change_index,
                  NULL,
                  0,
                  NULL);
  if (r < 0)
    {
      printf("Failed to apply kqueue changes: %s\n", strerror(errno));
    }
}

static MHD_Watch*
watch_new (const MHD_EventManager *api,
           int fd,
           MHD_WatchEvent events,
           MHD_WatchCallback callback,
           void *mhd_data)
{
  EventManager *em = (EventManager*) api->userdata;

  MHD_Watch *watch;
  if (NULL == (watch = malloc(sizeof(*watch))))
    return NULL;

  watch->fd = fd;
  watch->callback = callback;
  watch->em = em;
  watch->mhd_data = mhd_data;
  watch->next = NULL;
  watch->enable_read = 0;
  watch->enable_write = 0;
  watch->deleted = 0;

  watch_update (watch, events);
  return watch;
}

static void
watch_free (MHD_Watch *watch) {
  struct kevent change_set[2];
  int change_index = 0;

  if (watch->enable_read)
    {
      EV_SET (&change_set[change_index], watch->fd, EVFILT_READ, EV_DELETE, 0, 0, watch);
      change_index++;
    }

  if (watch->enable_write)
    {
      EV_SET (&change_set[change_index], watch->fd, EVFILT_WRITE, EV_DELETE, 0, 0, watch);
      change_index++;
    }

  if (change_index)
    {
      int r = kevent (watch->em->kqueue_fd,
                      (struct kevent*) &change_set,
                      change_index,
                      NULL,
                      0,
                      NULL);
      if (r < 0)
        {
          printf("Failed to apply kqueue changes: %s\n", strerror (errno));
        }
    }

  watch->deleted = 1;
  if (watch->em->orphaned_watches)
    {
      watch->next = watch->em->orphaned_watches;
      watch->em->orphaned_watches = watch;
    }
    else
    {
      watch->em->orphaned_watches = watch;
    }
}

static MHD_Timeout*
timeout_new (const MHD_EventManager *api,
             const struct timeval *tv,
             MHD_TimeoutCallback callback,
             void *mhd_data)
{
  EventManager *em = (EventManager*) api->userdata;

  MHD_Timeout *timeout;
  if (NULL == (timeout = malloc (sizeof(*timeout))))
    return NULL;

  timeout->callback = callback;
  timeout->em = em;
  timeout->trigger_time = *tv;
  timeout->mhd_data = mhd_data;

  /* Add the timeout to the timeout list */
  if (!em->timeout_head)
    {
      em->timeout_head = timeout;
      em->timeout_tail = timeout;
      timeout->next = NULL;
      timeout->previous = NULL;
    }
    else
    {
      timeout->next = NULL;
      timeout->previous = em->timeout_tail;
      em->timeout_tail = timeout;
    }

  return timeout;
}

static void
timeout_free (MHD_Timeout *timeout)
{
  if (timeout->em->timeout_head == timeout &&
      timeout->em->timeout_tail == timeout)
    {
      /* list is now empty */
      timeout->em->timeout_head = NULL;
      timeout->em->timeout_tail = NULL;
    }
    else if (timeout->em->timeout_head == timeout)
    {
      /* head of the list */
      timeout->em->timeout_head = timeout->next;
      timeout->next->previous = NULL;
    }
    else if (timeout->em->timeout_tail == timeout)
    {
      /* tail of the list */
      timeout->previous->next = NULL;
      timeout->em->timeout_tail = timeout->previous;
    }
    else
    {
      /* in the middle */
      timeout->previous->next = timeout->next;
      timeout->next->previous = timeout->previous;
    }

  free(timeout);
}

static void
timeout_update (MHD_Timeout *timeout,
                const struct timeval *tv)
{
  if (tv)
    {
      timeout->trigger_time = *tv;
    }
    else
    {
      timeout->trigger_time.tv_sec = 0;
      timeout->trigger_time.tv_usec = 0;
    }
}

struct EventManager*
event_manager_init ()
{
  EventManager *em;
  if (NULL == (em = malloc(sizeof(*em))))
  {
    return NULL;
  }

  em->timeout_head = NULL;
  em->timeout_tail = NULL;
  em->orphaned_watches = NULL;
  em->stop = 0;

  em->kqueue_fd = kqueue ();
  if (em->kqueue_fd < 0)
    {
      free(em);
      return NULL;
    }

  em->interface = malloc(sizeof(em->interface));
  em->interface->userdata = em;
  em->interface->watch_new = watch_new;
  em->interface->watch_update = watch_update;
  em->interface->watch_free = watch_free;
  em->interface->timeout_new = timeout_new;
  em->interface->timeout_update = timeout_update;
  em->interface->timeout_free = timeout_free;
  return em;
}

int
event_manager_loop (EventManager *em) {
  em->stop = 0;
  enum { EVENT_SET_SIZE = 10 };

  if (em->kqueue_fd < 0)
    {
      return -1;
    }

  struct kevent events[EVENT_SET_SIZE];

  while (!em->stop)
    {
      struct timeval tv;
      int found = 0;
      MHD_Timeout *timeout = em->timeout_head;
      while (timeout != NULL)
        {
          if (timerisset(&timeout->trigger_time) &&
              (!found || timercmp(&timeout->trigger_time, &tv, <)))
            {
              tv = timeout->trigger_time;
              found = 1;
            }
          timeout = timeout->next;
        }
      struct timespec ts;
      if (found)
        {
          struct timeval now;
          gettimeofday(&now, NULL);
          if (timercmp(&tv, &now, <))
            {
              ts.tv_sec = 0;
              ts.tv_nsec = 0;
            }
            else
            {
              struct timeval remaining;
              timersub(&tv, &now, &remaining);
              ts.tv_sec = remaining.tv_sec;
              ts.tv_nsec = remaining.tv_usec * 1000;
            }
        }
        else
        {
          ts.tv_sec = 1;
          ts.tv_nsec = 0;
        }

      int ready = kevent (em->kqueue_fd, NULL, 0,
                          events, EVENT_SET_SIZE, &ts);

      if (ready == 0)
        {
          struct timeval now;
          gettimeofday(&now, NULL);
          timeout = em->timeout_head;
          while (timeout != NULL)
          {
            if (timercmp(&timeout->trigger_time, &now, <))
              {
                MHD_Timeout *temp = timeout;
                timeout = timeout->next;
                temp->trigger_time.tv_sec = 0;
                temp->trigger_time.tv_usec = 0;
                temp->callback(temp, temp->mhd_data);
              }
              else
              {
                timeout = timeout->next;
              }
          }
        }
        else if (ready == -1)
        {
          if (errno == EINTR)
            continue;

          printf ("kevent() error: %s\n", strerror (errno));
          return -1;
        }

      for (int i = 0; i < ready; i++)
        {
          if (events[i].flags & EV_ERROR)
            {
              printf ("kevent error, fd: %ld, err: %s\n", events[i].ident,
                      strerror (events[i].data));
            }
            else
            {
              MHD_Watch *watch = (MHD_Watch*) events[i].udata;
              if (!watch->deleted && events[i].filter == EVFILT_READ)
                {
                  watch->callback (watch, watch->fd, MHD_WATCH_IN,
                                   watch->mhd_data);
                }

              if (!watch->deleted && events[i].filter == EVFILT_WRITE)
                {
                  watch->callback (watch, watch->fd, MHD_WATCH_OUT,
                                   watch->mhd_data);
                }
            }
        }

      MHD_Watch *watch = em->orphaned_watches;
      while (NULL != watch)
        {
          MHD_Watch *tmp = watch;
          watch = watch->next;
          free(tmp);
        }
      em->orphaned_watches = NULL;
    }

  close (em->kqueue_fd);
  em->kqueue_fd = -1;
  return 0;
}

void
event_manager_stop (EventManager *em) {
  em->stop = 1;
}

MHD_EventManager*
event_manager_interface(struct EventManager *em)
{
  return em->interface;
}

void
event_manager_cleanup (struct EventManager *em) {
  free(em->interface);
  free(em);
}
