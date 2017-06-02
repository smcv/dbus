/* containers.c - restricted bus servers for containers
 *
 * Copyright © 2017 Collabora Ltd.
 *
 * Licensed under the Academic Free License version 2.1
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <config.h>
#include "containers.h"

#ifdef DBUS_ENABLE_CONTAINERS

#ifndef DBUS_UNIX
# error DBUS_ENABLE_CONTAINERS requires DBUS_UNIX
#endif

dbus_bool_t
bus_containers_handle_add_server (DBusConnection *connection,
                                  BusTransaction *transaction,
                                  DBusMessage    *message,
                                  DBusError      *error)
{
  dbus_set_error (error, DBUS_ERROR_NOT_SUPPORTED, "Not yet implemented");
  return FALSE;
}

dbus_bool_t
bus_containers_supported_arguments_getter (BusContext *context,
                                           DBusMessageIter *var_iter)
{
  DBusMessageIter arr_iter;

  /* There are none so far */
  return dbus_message_iter_open_container (var_iter, DBUS_TYPE_ARRAY,
                                           DBUS_TYPE_STRING_AS_STRING,
                                           &arr_iter) &&
         dbus_message_iter_close_container (var_iter, &arr_iter);
}

#endif /* DBUS_ENABLE_CONTAINERS */
