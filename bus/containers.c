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

#include "dbus/dbus-internals.h"

#ifdef HAVE_UNIX_FD_PASSING
#include "dbus/dbus-hash.h"
#include "dbus/dbus-message-internal.h"
#include "dbus/dbus-server-socket.h"
#include "dbus/dbus-sysdeps-unix.h"

#include "connection.h"
#include "utils.h"

struct BusContainers
{
  int refcount;
  /* path borrowed from BusContainerInstance => owned BusContainerInstance */
  DBusHashTable *instances_by_path;
};

BusContainers *
bus_containers_new (void)
{
  /* We allocate the hash table lazily, expecting that the common case will
   * be a connection where this feature is never used */
  BusContainers *self = dbus_new0 (BusContainers, 1);

  if (self == NULL)
    return NULL;

  self->refcount = 1;
  return self;
}

void
bus_containers_unref (BusContainers *self)
{
  _dbus_assert (self != NULL);

  if (--self->refcount == 0)
    {
      if (self->instances_by_path != NULL)
        _dbus_hash_table_unref (self->instances_by_path);

      dbus_free (self);
    }
}

typedef struct
{
  int refcount;
  char *path;
  char *type;
  char *name;
  DBusVariant *metadata;
  DBusServer *server;
} BusContainerInstance;

static dbus_uint64_t next_container_id = 0;

static BusContainerInstance *
bus_container_instance_new (DBusError *error)
{
  BusContainerInstance *self = NULL;
  DBusString path;

  if (!_dbus_string_init (&path))
    {
      BUS_SET_OOM (error);
      return NULL;
    }

  self = dbus_new0 (BusContainerInstance, 1);

  if (self == NULL)
    {
      BUS_SET_OOM (error);
      goto fail;
    }

  if (next_container_id >= DBUS_UINT64_CONSTANT (0xFFFFFFFFFFFFFFFF))
    {
      /* We can't increment it any further without wrapping around */
      dbus_set_error (error, DBUS_ERROR_LIMITS_EXCEEDED,
                      "Too many containers created during the lifetime of "
                      "this bus");
      goto fail;
    }

  if (!_dbus_string_append_printf (&path,
                                   "/org/freedesktop/DBus/Containers/c%" PRIu64,
                                   next_container_id++))
    {
      BUS_SET_OOM (error);
      goto fail;
    }

  if (!_dbus_string_steal_data (&path, &self->path))
    goto fail;

  self->refcount = 1;
  self->type = NULL;
  self->name = NULL;
  self->metadata = NULL;
  self->server = NULL;
  return self;

fail:
  _dbus_string_free (&path);
  dbus_free (self);
  return NULL;
}

static BusContainerInstance *
bus_container_instance_ref (BusContainerInstance *self)
{
  _dbus_assert (self->refcount > 0);

  self->refcount++;
  return self;
}

static void
bus_container_instance_unref (BusContainerInstance *self)
{
  _dbus_assert (self->refcount > 0);

  if (--self->refcount == 0)
    {
      dbus_free (self->path);
      dbus_free (self->type);
      dbus_free (self->name);

      if (self->metadata != NULL)
        _dbus_variant_free (self->metadata);

      if (self->server != NULL)
        {
          dbus_server_disconnect (self->server);
          dbus_server_unref (self->server);
        }
    }
}

/* Free-function for DBusHash, which calls the free-function on NULL even
 * if there is no NULL in the hash table */
static void
bus_container_instance_unref0 (void *p)
{
  if (p != NULL)
    bus_container_instance_unref (p);
}

dbus_bool_t
bus_containers_handle_add_container_server (DBusConnection *connection,
                                            BusTransaction *transaction,
                                            DBusMessage    *message,
                                            DBusError      *error)
{
  DBusMessageIter iter;
  DBusMessageIter dict_iter;
  const char *type;
  const char *name;
  BusContainerInstance *instance = NULL;
  DBusString address;
  dbus_bool_t address_inited = FALSE;
  DBusSocket sock = DBUS_SOCKET_INIT;
  BusContext *context;
  BusContainers *containers;

  context = bus_transaction_get_context (transaction);
  containers = bus_context_get_containers (context);

  if (!_dbus_string_init (&address))
    goto oom;

  address_inited = TRUE;

  instance = bus_container_instance_new (error);

  if (instance == NULL)
    goto fail;

  /* We already checked this in bus_driver_handle_message() */
  _dbus_assert (dbus_message_has_signature (message, "ssa{sv}ha{sv}"));

  /* Argument 0: Container type */
  if (!dbus_message_iter_init (message, &iter))
    _dbus_assert_not_reached ("Message type was already checked");

  _dbus_assert (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_STRING);
  dbus_message_iter_get_basic (&iter, &type);
  instance->type = _dbus_strdup (type);

  if (!dbus_validate_interface (type, NULL))
    {
      dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                      "The container type identifier must have the "
                      "syntax of an interface name");
      goto fail;
    }

  /* Argument 1: Name as defined by container manager */
  if (!dbus_message_iter_next (&iter))
    _dbus_assert_not_reached ("Message type was already checked");

  _dbus_assert (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_STRING);
  dbus_message_iter_get_basic (&iter, &name);
  instance->name = _dbus_strdup (name);

  /* Argument 2: Metadata as defined by container manager */
  if (!dbus_message_iter_next (&iter))
    _dbus_assert_not_reached ("Message type was already checked");

  _dbus_assert (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_ARRAY);
  instance->metadata = _dbus_variant_read (&iter);
  _dbus_assert (strcmp (_dbus_variant_get_signature (instance->metadata),
                        "a{sv}") == 0);

  /* Argument 3: Socket to accept */
  if (!dbus_message_iter_next (&iter))
    _dbus_assert_not_reached ("Message type was already checked");

  _dbus_assert (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_UNIX_FD);
  dbus_message_iter_get_basic (&iter, &sock.fd);

  if (sock.fd < 0)
    {
      dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                      "Unable to retrieve file descriptor from message");
      goto fail;
    }

  /* Argument 4: Named parameters */
  if (!dbus_message_iter_next (&iter))
    _dbus_assert_not_reached ("Message type was already checked");

  _dbus_assert (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_ARRAY);
  dbus_message_iter_recurse (&iter, &dict_iter);

  while (dbus_message_iter_get_arg_type (&dict_iter) != DBUS_TYPE_INVALID)
    {
      DBusMessageIter pair_iter;
      const char *param_name;

      _dbus_assert (dbus_message_iter_get_arg_type (&dict_iter) ==
                    DBUS_TYPE_DICT_ENTRY);

      dbus_message_iter_recurse (&dict_iter, &pair_iter);
      _dbus_assert (dbus_message_iter_get_arg_type (&pair_iter) ==
                    DBUS_TYPE_STRING);
      dbus_message_iter_get_basic (&pair_iter, &param_name);

      /* If we supported any named parameters, we'd copy them into the data
       * structure here; but we don't, so fail instead. */
      dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                      "Named parameter %s is not understood", param_name);
      goto fail;
    }

  /* End of arguments */
  _dbus_assert (!dbus_message_iter_has_next (&iter));

  if (!_dbus_append_address_from_socket (sock, &address, error))
    goto fail;

  instance->server = _dbus_server_new_for_socket (&sock, 1, &address, NULL,
                                                  error);

  if (instance->server == NULL)
    goto fail;

  _dbus_socket_invalidate (&sock); /* transfer ownership if we succeeded */

  if (containers->instances_by_path == NULL)
    {
      containers->instances_by_path = _dbus_hash_table_new (DBUS_HASH_STRING,
                                                            NULL,
                                                            bus_container_instance_unref0);

      if (containers->instances_by_path == NULL)
        goto oom;
    }

  if (_dbus_hash_table_insert_string (containers->instances_by_path,
                                      instance->path, instance))
    bus_container_instance_ref (instance);
  else
    goto oom;

  /* TODO: Actually implement the method */
  dbus_set_error (error, DBUS_ERROR_NOT_SUPPORTED, "Not yet implemented");
  goto fail;

oom:
  BUS_SET_OOM (error);
  /* fall through */
fail:
  /* It's OK to try to remove the instance from the hash table even if we
   * got an error before we added it, because they all have unique object
   * paths anyway. */
  if (instance != NULL && instance->path != NULL &&
      containers->instances_by_path != NULL)
    _dbus_hash_table_remove_string (containers->instances_by_path,
                                    instance->path);

  if (instance != NULL)
    bus_container_instance_unref (instance);

  if (address_inited)
    _dbus_string_free (&address);

  if (_dbus_socket_is_valid (sock))
    _dbus_close_socket (sock, NULL);

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

#else

BusContainers *
bus_containers_new (void)
{
  /* Return an arbitrary non-NULL pointer just to indicate that we didn't
   * fail. There is no valid operation to do with it on this platform,
   * other than unreffing it, which does nothing. */
  return (BusContainers *) 1;
}

void
bus_containers_unref (BusContainers *self)
{
  _dbus_assert (self == (BusContainers *) 1);
}

#endif /* HAVE_UNIX_FD_PASSING */
