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

/* Data attached to a DBusConnection that has created container instances. */
typedef struct
{
  /* List of instances created by this container manager */
  DBusList *instances;
} BusContainerManagerData;

/* Data slot on DBusConnection, holding BusContainerManagerData */
static dbus_int32_t container_manager_data_slot = -1;

/* Data slot on DBusConnection, holding BusContainerInstance */
static dbus_int32_t contained_data_slot = -1;

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
  BusContainers *self;

  if (!dbus_connection_allocate_data_slot (&container_manager_data_slot))
    return NULL;

  if (!dbus_connection_allocate_data_slot (&contained_data_slot))
    return NULL;

  self = dbus_new0 (BusContainers, 1);

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
  BusContext *context;
} BusContainerInstance;

static dbus_uint64_t next_container_id = 0;

static BusContainerInstance *
bus_container_instance_new (BusContext *context,
                            DBusError *error)
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
  self->context = bus_context_ref (context);
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
bus_container_instance_stop_listening (BusContainerInstance *self)
{
  if (self->server != NULL)
    {
      dbus_server_set_new_connection_function (self->server, NULL, NULL, NULL);
      dbus_server_disconnect (self->server);
      dbus_server_unref (self->server);
      self->server = NULL;
    }
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

      bus_container_instance_stop_listening (self);

      if (self->context != NULL)
        bus_context_unref (self->context);
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

static void
bus_container_manager_data_free (BusContainerManagerData *self)
{
  BusContainerInstance *instance;

  while ((instance = _dbus_list_pop_first (&self->instances)) != NULL)
    bus_container_instance_unref (instance);

  dbus_free (self);
}

/* We only accept the best available auth mechanism */
static const char * const auth_mechanisms[] =
{
  "EXTERNAL",
  NULL
};

static void
new_connection_cb (DBusServer     *server,
                   DBusConnection *new_connection,
                   void           *data)
{
  BusContainerInstance *instance = data;

  if (!dbus_connection_set_data (new_connection, contained_data_slot,
                                 bus_container_instance_ref (instance),
                                 (DBusFreeFunction) bus_container_instance_unref))
    {
      bus_container_instance_unref (instance);
      return;
    }

  if (!bus_context_add_incoming_connection (instance->context, new_connection))
    return;

  dbus_connection_set_allow_anonymous (new_connection, FALSE);
}

dbus_bool_t
bus_containers_handle_add_container_server (DBusConnection *connection,
                                            BusTransaction *transaction,
                                            DBusMessage    *message,
                                            DBusError      *error)
{
  BusContainerManagerData *d;
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
  DBusList *link_for_instances = NULL;
  DBusMessage *reply = NULL;

  context = bus_transaction_get_context (transaction);
  containers = bus_context_get_containers (context);

  if (!_dbus_string_init (&address))
    goto oom;

  address_inited = TRUE;

  d = dbus_connection_get_data (connection, container_manager_data_slot);

  if (d == NULL)
    {
      d = dbus_new0 (BusContainerManagerData, 1);
      d->instances = NULL;

      if (!dbus_connection_set_data (connection, container_manager_data_slot,
                                     d,
                                     (DBusFreeFunction) bus_container_manager_data_free))
        goto oom;
    }

  instance = bus_container_instance_new (context, error);

  if (instance == NULL)
    goto fail;

  link_for_instances = _dbus_list_alloc_link (instance);

  if (link_for_instances == NULL)
    goto oom;

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

  if (!bus_context_setup_server (context, instance->server, error))
    goto fail;

  if (!dbus_server_set_auth_mechanisms (instance->server,
                                        (const char **) auth_mechanisms))
    goto oom;

  dbus_server_set_new_connection_function (instance->server, new_connection_cb,
                                           instance, NULL);

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

  _dbus_list_append_link (&d->instances, link_for_instances);
  /* transfer ownership */
  bus_container_instance_ref (instance);
  link_for_instances = NULL;

  reply = dbus_message_new_method_return (message);

  if (!dbus_message_append_args (reply,
                                 DBUS_TYPE_OBJECT_PATH, &instance->path,
                                 DBUS_TYPE_INVALID))
    goto oom;

  _dbus_assert (dbus_message_has_signature (reply, "o"));

  if (! bus_transaction_send_from_driver (transaction, connection, reply))
    goto oom;

  dbus_message_unref (reply);
  bus_container_instance_unref (instance);
  _dbus_string_free (&address);
  return TRUE;

oom:
  BUS_SET_OOM (error);
  /* fall through */
fail:
  if (reply != NULL)
    dbus_message_unref (reply);

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

  if (link_for_instances != NULL)
    _dbus_list_free_link (link_for_instances);

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

dbus_bool_t
bus_containers_connection_is_contained (DBusConnection *connection,
                                        const char **path,
                                        const char **type,
                                        const char **name)
{
#ifdef HAVE_UNIX_FD_PASSING
  BusContainerInstance *instance;

  instance = dbus_connection_get_data (connection, contained_data_slot);

  if (instance != NULL)
    {
      if (path != NULL)
        *path = instance->path;

      if (type != NULL)
        *type = instance->type;

      if (name != NULL)
        *name = instance->name;

      return TRUE;
    }
#endif

  return FALSE;
}

void
bus_containers_remove_connection (BusContainers *self,
                                  DBusConnection *connection)
{
#ifdef HAVE_UNIX_FD_PASSING
  BusContainerManagerData *d;
  BusContainerInstance *instance;

  d = dbus_connection_get_data (connection, container_manager_data_slot);

  if (d == NULL)
    return;

  while ((instance = _dbus_list_pop_first (&d->instances)) != NULL)
    {
      bus_container_instance_stop_listening (instance);
      bus_container_instance_unref (instance);
    }
#endif
}

void
bus_containers_shutdown (void)
{
#ifdef HAVE_UNIX_FD_PASSING
  dbus_connection_free_data_slot (&container_manager_data_slot);
  dbus_connection_free_data_slot (&contained_data_slot);
#endif
}
