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
 * @file event_manager.h
 * @brief The generic event manager interface
 * @author Simon Newton
 */

#ifndef EVENT_MANAGER_H
#define EVENT_MANAGER_H

#include <microhttpd.h>

/**
 * @brief A generic EventManager.
 *
 * Implementations using epoll and kevent are provided.
 */
typedef struct EventManager EventManager;

/**
 * @brief Create a new EventManager.
 * @returns A new EventManager, or NULL if one couldn't be created.
 */
EventManager*
event_manager_init ();

/**
 * @brief Enter the EventManager loop.
 * @param em An EventManager.
 * @returns 0 on clean exit, non-0 if an error occured.
 */
int
event_manager_loop (EventManager *em);

/**
 * @brief Stop the EventManager loop.
 * @param em An EventManager.
 */
void
event_manager_stop (EventManager *em);

/**
 * @brief Fetch a pointer to the MHD_EventManager.
 * @param em An EventManager.
 * @returns A MHD_EventManager to be passed to MHD_start_daemon.
 */
MHD_EventManager*
event_manager_interface(EventManager *em);

/**
 * @brief Destory an EventManager.
 * @param em An EventManager.
 */
void
event_manager_cleanup (EventManager *em);

#endif
