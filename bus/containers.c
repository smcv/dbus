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

#ifdef DBUS_ENABLE_CONTAINERS

#ifndef DBUS_UNIX
# error DBUS_ENABLE_CONTAINERS requires DBUS_UNIX
#endif

#include "dbus/dbus-hash.h"
#include "dbus/dbus-message-internal.h"
#include "dbus/dbus-sysdeps-unix.h"

#include "connection.h"
#include "utils.h"

/*
 * A container instance groups together a per-app-container server with
 * all the connections for which it is responsible.
 */
typedef struct
{
  int refcount;
  char *path;
  char *type;
  char *name;
  DBusVariant *metadata;
  BusContext *context;
  BusContainers *containers;
} BusContainerInstance;

/*
 * Singleton data structure encapsulating the container-related parts of
 * a BusContext.
 */
struct BusContainers
{
  int refcount;
  /* path borrowed from BusContainerInstance => unowned BusContainerInstance
   * The BusContainerInstance removes itself from here on destruction. */
  DBusHashTable *instances_by_path;
  dbus_uint64_t next_container_id;
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
  self->instances_by_path = NULL;
  self->next_container_id = DBUS_UINT64_CONSTANT (0);
  return self;
}

BusContainers *
bus_containers_ref (BusContainers *self)
{
  _dbus_assert (self->refcount > 0);
  _dbus_assert (self->refcount < _DBUS_INT_MAX);

  self->refcount++;
  return self;
}

void
bus_containers_unref (BusContainers *self)
{
  _dbus_assert (self != NULL);
  _dbus_assert (self->refcount > 0);

  if (--self->refcount == 0)
    {
      _dbus_clear_hash_table (&self->instances_by_path);
      dbus_free (self);
    }
}

static void
bus_container_instance_unref (BusContainerInstance *self)
{
  _dbus_assert (self->refcount > 0);

  if (--self->refcount == 0)
    {
      /* It's OK to do this even if we were never added to instances_by_path,
       * because the paths are globally unique. */
      if (self->path != NULL && self->containers->instances_by_path != NULL)
        _dbus_hash_table_remove_string (self->containers->instances_by_path,
                                        self->path);

      _dbus_clear_variant (&self->metadata);
      bus_context_unref (self->context);
      bus_containers_unref (self->containers);
      dbus_free (self->path);
      dbus_free (self->type);
      dbus_free (self->name);
      dbus_free (self);
    }
}

static inline void
bus_clear_container_instance (BusContainerInstance **instance_p)
{
  _dbus_clear_pointer_impl (BusContainerInstance, instance_p,
                            bus_container_instance_unref);
}

static BusContainerInstance *
bus_container_instance_new (BusContext *context,
                            BusContainers *containers,
                            DBusError *error)
{
  BusContainerInstance *self = NULL;
  DBusString path = _DBUS_STRING_INIT_INVALID;

  _dbus_assert (context != NULL);
  _dbus_assert (containers != NULL);
  _DBUS_ASSERT_ERROR_IS_CLEAR (error);

  if (!_dbus_string_init (&path))
    {
      BUS_SET_OOM (error);
      goto fail;
    }

  self = dbus_new0 (BusContainerInstance, 1);

  if (self == NULL)
    {
      BUS_SET_OOM (error);
      goto fail;
    }

  self->refcount = 1;
  self->type = NULL;
  self->name = NULL;
  self->metadata = NULL;
  self->context = bus_context_ref (context);
  self->containers = bus_containers_ref (containers);

  if (containers->next_container_id >=
      DBUS_UINT64_CONSTANT (0xFFFFFFFFFFFFFFFF))
    {
      /* We can't increment it any further without wrapping around */
      dbus_set_error (error, DBUS_ERROR_LIMITS_EXCEEDED,
                      "Too many containers created during the lifetime of "
                      "this bus");
      goto fail;
    }

  /* We assume PRIu64 exists on all Unix platforms: it's ISO C99, and the
   * only non-C99 platform we support is MSVC on Windows. */
  if (!_dbus_string_append_printf (&path,
                                   "/org/freedesktop/DBus/Containers1/c%" PRIu64,
                                   containers->next_container_id++))
    {
      BUS_SET_OOM (error);
      goto fail;
    }

  if (!_dbus_string_steal_data (&path, &self->path))
    goto fail;

  return self;

fail:
  _dbus_string_free (&path);

  if (self != NULL)
    bus_container_instance_unref (self);

  return NULL;
}

dbus_bool_t
bus_containers_handle_add_server (DBusConnection *connection,
                                  BusTransaction *transaction,
                                  DBusMessage    *message,
                                  DBusError      *error)
{
  DBusMessageIter iter;
  DBusMessageIter dict_iter;
  const char *type;
  const char *name;
  BusContainerInstance *instance = NULL;
  BusContext *context;
  BusContainers *containers;

  context = bus_transaction_get_context (transaction);
  containers = bus_context_get_containers (context);

  instance = bus_container_instance_new (context, containers, error);

  if (instance == NULL)
    goto fail;

  /* We already checked this in bus_driver_handle_message() */
  _dbus_assert (dbus_message_has_signature (message, "ssa{sv}a{sv}"));

  /* Argument 0: Container type */
  if (!dbus_message_iter_init (message, &iter))
    _dbus_assert_not_reached ("Message type was already checked");

  _dbus_assert (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_STRING);
  dbus_message_iter_get_basic (&iter, &type);
  instance->type = _dbus_strdup (type);

  if (instance->type == NULL)
    goto oom;

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

  if (instance->name == NULL)
    goto oom;

  /* Argument 2: Metadata as defined by container manager */
  if (!dbus_message_iter_next (&iter))
    _dbus_assert_not_reached ("Message type was already checked");

  _dbus_assert (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_ARRAY);
  instance->metadata = _dbus_variant_read (&iter);
  _dbus_assert (strcmp (_dbus_variant_get_signature (instance->metadata),
                        "a{sv}") == 0);

  /* Argument 3: Named parameters */
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

  if (containers->instances_by_path == NULL)
    {
      containers->instances_by_path = _dbus_hash_table_new (DBUS_HASH_STRING,
                                                            NULL, NULL);

      if (containers->instances_by_path == NULL)
        goto oom;
    }

  if (!_dbus_hash_table_insert_string (containers->instances_by_path,
                                       instance->path, instance))
    goto oom;

  /* TODO: Actually implement the method */
  dbus_set_error (error, DBUS_ERROR_NOT_SUPPORTED, "Not yet implemented");
  goto fail;

oom:
  BUS_SET_OOM (error);
  /* fall through */
fail:

  bus_clear_container_instance (&instance);
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

BusContainers *
bus_containers_ref (BusContainers *self)
{
  _dbus_assert (self == (BusContainers *) 1);
  return self;
}

void
bus_containers_unref (BusContainers *self)
{
  _dbus_assert (self == (BusContainers *) 1);
}

#endif /* DBUS_ENABLE_CONTAINERS */
