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
  DBusServer *server;
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
  DBusString address_template;
  dbus_uint64_t next_container_id;
};

BusContainers *
bus_containers_new (void)
{
  /* We allocate the hash table lazily, expecting that the common case will
   * be a connection where this feature is never used */
  BusContainers *self = dbus_new0 (BusContainers, 1);
  DBusString invalid = _DBUS_STRING_INIT_INVALID;

  if (self == NULL)
    goto oom;

  self->refcount = 1;
  self->instances_by_path = NULL;
  self->next_container_id = DBUS_UINT64_CONSTANT (0);
  self->address_template = invalid;

  /* Make it mutable */
  if (!_dbus_string_init (&self->address_template))
    goto oom;

  if (_dbus_getuid () == 0)
    {
      DBusString dir;

      /* System bus (we haven't dropped privileges at this point), or
       * root's session bus. Use random socket paths resembling
       * /run/dbus/containers/dbus-abcdef, which is next to /run/dbus/pid
       * (if not using the Red Hat init scripts, which use a different
       * pid file for historical reasons).
       *
       * We rely on the tmpfiles.d snippet or an OS-specific init script to
       * have created this directory with the appropriate owner; if it hasn't,
       * creating container sockets will just fail. */
      _dbus_string_init_const (&dir, DBUS_RUNSTATEDIR "/dbus/containers");

      /* We specifically use paths, because an abstract socket that you can't
       * bind-mount is not particularly useful. */
      if (!_dbus_string_append (&self->address_template, "unix:dir=") ||
          !_dbus_address_append_escaped (&self->address_template, &dir))
        goto oom;
    }
  else
    {
      /* Otherwise defer creating the directory for sockets until we need it,
       * so that we can have better error behaviour */
    }

  return self;

oom:
  bus_clear_containers (&self);

  return NULL;
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
      _dbus_string_free (&self->address_template);
      dbus_free (self);
    }
}

static BusContainerInstance *
bus_container_instance_ref (BusContainerInstance *self)
{
  _dbus_assert (self->refcount > 0);
  _dbus_assert (self->refcount < _DBUS_INT_MAX);

  self->refcount++;
  return self;
}

static void
bus_container_instance_unref (BusContainerInstance *self)
{
  _dbus_assert (self->refcount > 0);

  if (--self->refcount == 0)
    {
      /* As long as the server is listening, the BusContainerInstance can't
       * be freed, because the DBusServer holds a reference to the
       * BusContainerInstance */
      _dbus_assert (self->server == NULL);

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

static void
bus_container_instance_stop_listening (BusContainerInstance *self)
{
  /* In case the DBusServer holds the last reference to self */
  bus_container_instance_ref (self);

  if (self->server != NULL)
    {
      dbus_server_set_new_connection_function (self->server, NULL, NULL, NULL);
      dbus_server_disconnect (self->server);
      dbus_clear_server (&self->server);
    }

  bus_container_instance_unref (self);
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
  self->server = NULL;

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

/* We only accept EXTERNAL authentication, because Unix platforms that are
 * sufficiently capable to have app-containers ought to have it. */
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

  /* If this fails it logs a warning, so we don't need to do that */
  if (!bus_context_add_incoming_connection (instance->context, new_connection))
    return;
}

static const char *
bus_containers_ensure_address_template (BusContainers *self,
                                        DBusError     *error)
{
  DBusString dir = _DBUS_STRING_INIT_INVALID;
  const char *ret = NULL;
  const char *runtime_dir;

  /* Early-return if we already did this */
  if (_dbus_string_get_length (&self->address_template) > 0)
    return _dbus_string_get_const_data (&self->address_template);

  runtime_dir = _dbus_getenv ("XDG_RUNTIME_DIR");

  if (runtime_dir != NULL)
    {
      if (!_dbus_string_init (&dir))
        {
          BUS_SET_OOM (error);
          return FALSE;
        }

      /* We listen on a random socket path resembling
       * /run/user/1000/dbus-1/containers/dbus-abcdef, chosen to share
       * the dbus-1 directory with the dbus-1/services used for transient
       * session services. */
      if (!_dbus_string_append (&dir, runtime_dir) ||
          !_dbus_string_append (&dir, "/dbus-1"))
        {
          BUS_SET_OOM (error);
          goto out;
        }

      if (!_dbus_ensure_directory (&dir, error))
        goto out;

      if (!_dbus_string_append (&dir, "/containers"))
        {
          BUS_SET_OOM (error);
          goto out;
        }

      if (!_dbus_ensure_directory (&dir, error))
        goto out;
    }
  else
    {
      /* No XDG_RUNTIME_DIR, so don't do anything special or clever: just
       * use a random socket like /tmp/dbus-abcdef. */
      const char *tmpdir;

      tmpdir = _dbus_get_tmpdir ();
      _dbus_string_init_const (&dir, tmpdir);
    }

  /* We specifically use paths, even on Linux (unix:dir= not unix:tmpdir=),
   * because an abstract socket that you can't bind-mount is not useful
   * when you want something you can bind-mount into a container. */
  if (!_dbus_string_append (&self->address_template, "unix:dir=") ||
      !_dbus_address_append_escaped (&self->address_template, &dir))
    {
      _dbus_string_set_length (&self->address_template, 0);
      BUS_SET_OOM (error);
      goto out;
    }

  ret = _dbus_string_get_const_data (&self->address_template);

out:
  _dbus_string_free (&dir);
  return ret;
}

static dbus_bool_t
bus_container_instance_listen (BusContainerInstance *self,
                               DBusError            *error)
{
  BusContainers *containers = bus_context_get_containers (self->context);
  const char *address;

  address = bus_containers_ensure_address_template (containers, error);

  if (address == NULL)
    return FALSE;

  self->server = dbus_server_listen (address, error);

  if (self->server == NULL)
    return FALSE;

  if (!bus_context_setup_server (self->context, self->server, error))
    return FALSE;

  if (!dbus_server_set_auth_mechanisms (self->server,
                                        (const char **) auth_mechanisms))
    {
      BUS_SET_OOM (error);
      return FALSE;
    }

  /* Cannot fail because the memory it uses was already allocated */
  dbus_server_set_new_connection_function (self->server, new_connection_cb,
                                           bus_container_instance_ref (self),
                                           (DBusFreeFunction) bus_container_instance_unref);
  return TRUE;
}

dbus_bool_t
bus_containers_handle_add_server (DBusConnection *connection,
                                  BusTransaction *transaction,
                                  DBusMessage    *message,
                                  DBusError      *error)
{
  DBusMessageIter iter;
  DBusMessageIter dict_iter;
  DBusMessageIter writer;
  DBusMessageIter array_writer;
  const char *type;
  const char *name;
  const char *path;
  BusContainerInstance *instance = NULL;
  BusContext *context;
  BusContainers *containers;
  char *address = NULL;
  DBusAddressEntry **entries = NULL;
  int n_entries;
  DBusMessage *reply = NULL;

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

  /* This part is separated out because we eventually want to be able to
   * accept a fd-passed server socket in the named parameters, instead of
   * creating our own server, and defer listening on it until later */
  if (!bus_container_instance_listen (instance, error))
    goto fail;

  address = dbus_server_get_address (instance->server);

  if (!dbus_parse_address (address, &entries, &n_entries, error))
    _dbus_assert_not_reached ("listening on unix:dir= should yield a valid address");

  _dbus_assert (n_entries == 1);
  _dbus_assert (strcmp (dbus_address_entry_get_method (entries[0]), "unix") == 0);

  path = dbus_address_entry_get_value (entries[0], "path");
  _dbus_assert (path != NULL);

  reply = dbus_message_new_method_return (message);

  if (!dbus_message_append_args (reply,
                                 DBUS_TYPE_OBJECT_PATH, &instance->path,
                                 DBUS_TYPE_INVALID))
    goto oom;

  dbus_message_iter_init_append (reply, &writer);

  if (!dbus_message_iter_open_container (&writer, DBUS_TYPE_ARRAY,
                                         DBUS_TYPE_BYTE_AS_STRING,
                                         &array_writer))
    goto oom;

  if (!dbus_message_iter_append_fixed_array (&array_writer, DBUS_TYPE_BYTE,
                                             &path, strlen (path) + 1))
    {
      dbus_message_iter_abandon_container (&writer, &array_writer);
      goto oom;
    }

  if (!dbus_message_iter_close_container (&writer, &array_writer))
    goto oom;

  if (!dbus_message_append_args (reply,
                                 DBUS_TYPE_STRING, &address,
                                 DBUS_TYPE_INVALID))
    goto oom;

  _dbus_assert (dbus_message_has_signature (reply, "oays"));

  if (! bus_transaction_send_from_driver (transaction, connection, reply))
    goto oom;

  dbus_message_unref (reply);
  bus_container_instance_unref (instance);
  dbus_address_entries_free (entries);
  dbus_free (address);
  return TRUE;

oom:
  BUS_SET_OOM (error);
  /* fall through */
fail:
  if (instance != NULL)
    bus_container_instance_stop_listening (instance);

  dbus_clear_message (&reply);
  dbus_clear_address_entries (&entries);
  bus_clear_container_instance (&instance);
  dbus_free (address);
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

void
bus_containers_stop_listening (BusContainers *self)
{
  if (self->instances_by_path != NULL)
    {
      DBusHashIter iter;

      _dbus_hash_iter_init (self->instances_by_path, &iter);

      while (_dbus_hash_iter_next (&iter))
        {
          BusContainerInstance *instance = _dbus_hash_iter_get_value (&iter);

          bus_container_instance_stop_listening (instance);
        }
    }
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

void
bus_containers_stop_listening (BusContainers *self)
{
  _dbus_assert (self == (BusContainers *) 1);
}

#endif /* DBUS_ENABLE_CONTAINERS */
