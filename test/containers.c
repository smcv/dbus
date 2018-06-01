/* Integration tests for restricted sockets for containers
 *
 * Copyright © 2017-2018 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <config.h>

#include <errno.h>

#include <dbus/dbus.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#if defined(DBUS_ENABLE_CONTAINERS) && defined(HAVE_GIO_UNIX)

#define HAVE_CONTAINERS_TEST

/* For g_open() which is a #define based on open() */
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <gio/gunixfdlist.h>
#include <gio/gunixsocketaddress.h>

#include "dbus/dbus-sysdeps-unix.h"

#endif

#include "test-utils-glib.h"

#define BUS_INTERFACE_STATS "org.freedesktop.DBus.Debug.Stats"

typedef enum
{
  NAME_TRISTATE_MAYBE_OWNED = '?',
  NAME_TRISTATE_OWNED = '+',
  NAME_TRISTATE_NOT_OWNED = '-'
} NameTristate;

typedef struct {
    TestMainContext *ctx;
    gboolean skip;
    gchar *bus_address;
    GPid daemon_pid;
    GError *error;

    GDBusProxy *proxy;

    gchar *instance_path;
    gchar *socket_path;
    gchar *socket_dbus_address;
    GDBusConnection *unconfined_conn;
    gchar *unconfined_unique_name;
    guint unconfined_filter;
    GDBusConnection *confined_conns[2];
    gchar *confined_unique_names[2];
    guint confined_filters[2];

    GDBusConnection *observer_conn;
    GDBusProxy *observer_proxy;
    gchar *observer_unique_name;
    guint observer_filter;
    GHashTable *containers_removed;
    guint removed_sub;
    DBusConnection *libdbus_observer;
    DBusMessage *latest_shout;

    /* These watch the observer, from the perspective of the unconfined
     * connection. */
    NameTristate observer_unique_name_owned;
    NameTristate observer_well_known_name_owned;
    guint observer_unique_name_watch;
    guint observer_well_known_name_watch;

    /* First confined connection's subscription to NameOwnerChanged */
    guint confined_0_noc_sub;
    /* Queue of NameOwnerChange */
    GQueue name_owner_changes;
    NameTristate confined_1_name_owned;
} Fixture;

typedef struct
{
  gchar *name;
  gchar *old_owner;
  gchar *new_owner;
} NameOwnerChange;

static void
name_owner_change_free (NameOwnerChange *self)
{
  g_free (self->name);
  g_free (self->old_owner);
  g_free (self->new_owner);
  g_free (self);
}

typedef struct
{
  const gchar *config_file;
  enum
    {
      STOP_SERVER_EXPLICITLY,
      STOP_SERVER_DISCONNECT_FIRST,
      STOP_SERVER_NEVER_CONNECTED,
      STOP_SERVER_FORCE,
      STOP_SERVER_WITH_MANAGER
    }
  stop_server;
} Config;

static const Config default_config =
{
  NULL,
  0 /* not used, the stop-server test always uses non-default config */
};

#ifdef DBUS_ENABLE_CONTAINERS
/* A GDBusNameVanishedCallback that sets a boolean flag. */
static void
name_gone_set_boolean_cb (GDBusConnection *conn,
                          const gchar *name,
                          gpointer user_data)
{
  gboolean *gone_p = user_data;

  g_assert_nonnull (gone_p);
  g_assert_false (*gone_p);
  *gone_p = TRUE;
}
#endif

#ifdef HAVE_CONTAINERS_TEST
static void
iterate_both_main_loops (TestMainContext *ctx)
{
  /* TODO: Gluing these two main loops together so they can block would
   * be better than sleeping, but do we have enough API to do that without
   * reinventing dbus-glib? */
  g_usleep (G_USEC_PER_SEC / 100);
  test_main_context_iterate (ctx, FALSE);
  g_main_context_iteration (NULL, FALSE);
}
#endif

static DBusHandlerResult
observe_shouting_cb (DBusConnection *observer,
                     DBusMessage *message,
                     void *user_data)
{
  Fixture *f = user_data;

  if (dbus_message_is_signal (message, "com.example.Shouting", "Shouted"))
    {
      dbus_clear_message (&f->latest_shout);
      f->latest_shout = dbus_message_ref (message);
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
instance_removed_cb (GDBusConnection *observer,
                     const gchar *sender,
                     const gchar *path,
                     const gchar *iface,
                     const gchar *member,
                     GVariant *parameters,
                     gpointer user_data)
{
  Fixture *f = user_data;
  const gchar *container;

  g_assert_cmpstr (sender, ==, DBUS_SERVICE_DBUS);
  g_assert_cmpstr (path, ==, DBUS_PATH_DBUS);
  g_assert_cmpstr (iface, ==, DBUS_INTERFACE_CONTAINERS1);
  g_assert_cmpstr (member, ==, "InstanceRemoved");
  g_assert_cmpstr (g_variant_get_type_string (parameters), ==, "(o)");
  g_variant_get (parameters, "(&o)", &container);
  g_assert (!g_hash_table_contains (f->containers_removed, container));
  g_hash_table_add (f->containers_removed, g_strdup (container));
}

static void
fixture_disconnect_unconfined (Fixture *f)
{
  if (f->observer_unique_name_watch != 0)
    {
      g_bus_unwatch_name (f->observer_unique_name_watch);
      f->observer_unique_name_watch = 0;
    }

  if (f->observer_well_known_name_watch != 0)
    {
      g_bus_unwatch_name (f->observer_well_known_name_watch);
      f->observer_well_known_name_watch = 0;
    }

  if (f->unconfined_conn != NULL)
    {
      GError *error = NULL;

      if (f->unconfined_filter != 0)
        g_dbus_connection_remove_filter (f->unconfined_conn,
                                         f->unconfined_filter);

      g_dbus_connection_close_sync (f->unconfined_conn, NULL, &error);

      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CLOSED))
        g_clear_error (&error);
      else
        g_assert_no_error (error);
    }

  g_clear_object (&f->unconfined_conn);
}

static void
fixture_disconnect_observer (Fixture *f)
{
  if (f->observer_conn != NULL)
    {
      GError *error = NULL;

      g_dbus_connection_signal_unsubscribe (f->observer_conn,
                                            f->removed_sub);

      if (f->observer_filter != 0)
        g_dbus_connection_remove_filter (f->observer_conn,
                                         f->observer_filter);

      g_dbus_connection_close_sync (f->observer_conn, NULL, &error);

      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CLOSED))
        g_clear_error (&error);
      else
        g_assert_no_error (error);
    }

  g_clear_object (&f->observer_conn);
}

#ifdef HAVE_CONTAINERS_TEST
static void
fixture_connect_confined (Fixture *f,
                          guint i)
{
  GError *error = NULL;

  g_assert_cmpuint (i, <, G_N_ELEMENTS (f->confined_conns));
  g_assert_cmpuint (i, <, G_N_ELEMENTS (f->confined_unique_names));

  g_test_message ("Connecting to %s...", f->socket_dbus_address);
  f->confined_conns[i] = g_dbus_connection_new_for_address_sync (
      f->socket_dbus_address,
      (G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION |
       G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT),
      NULL, NULL, &error);
  g_assert_no_error (error);

  f->confined_unique_names[i] = g_strdup (
      g_dbus_connection_get_unique_name (f->confined_conns[i]));
}
#endif

static void
fixture_disconnect_confined (Fixture *f,
                             guint i)
{
  if (f->confined_conns[i] != NULL)
    {
      GError *error = NULL;

      if (i == 0 && f->confined_0_noc_sub != 0)
        g_dbus_connection_signal_unsubscribe (f->confined_conns[i],
                                              f->confined_0_noc_sub);

      if (f->confined_filters[i] != 0)
        g_dbus_connection_remove_filter (f->confined_conns[i],
                                         f->confined_filters[i]);

      g_dbus_connection_close_sync (f->confined_conns[i], NULL, &error);

      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CLOSED))
        g_clear_error (&error);
      else
        g_assert_no_error (error);
    }

  g_clear_object (&f->confined_conns[i]);
}

static void
observer_appeared_cb (GDBusConnection *connection,
                      const gchar *name,
                      const gchar *name_owner,
                      gpointer user_data)
{
  NameTristate *tristate = user_data;

  g_test_message ("Unconfined connection saw unconfined observer "
                  "connection \"%s\" appear, owned by \"%s\"",
                  name, name_owner);
  *tristate = NAME_TRISTATE_OWNED;
}

static void
observer_vanished_cb (GDBusConnection *connection,
                      const gchar *name,
                      gpointer user_data)
{
  NameTristate *tristate = user_data;

  g_test_message ("Unconfined connection saw unconfined observer "
                  "connection \"%s\" disappear",
                  name);
  *tristate = NAME_TRISTATE_NOT_OWNED;
}

#ifdef HAVE_CONTAINERS_TEST
/*
 * Helper for Allow tests: GDBusSignalCallback that adds
 * NameOwnerChanged signals to a queue.
 */
static void
confined_0_name_owner_changed_cb (GDBusConnection *subscriber,
                                  const gchar *sender,
                                  const gchar *sender_path,
                                  const gchar *iface,
                                  const gchar *member,
                                  GVariant *parameters,
                                  gpointer user_data)
{
  Fixture *f = user_data;
  NameOwnerChange *noc;

  g_assert (subscriber == f->confined_conns[0]);

  g_assert_cmpstr (sender, ==, DBUS_SERVICE_DBUS);
  g_assert_cmpstr (sender_path, ==, DBUS_PATH_DBUS);
  g_assert_cmpstr (iface, ==, DBUS_INTERFACE_DBUS);
  g_assert_cmpstr (member, ==, "NameOwnerChanged");

  noc = g_new0 (NameOwnerChange, 1);
  g_variant_get (parameters, "(sss)",
                 &noc->name, &noc->old_owner, &noc->new_owner);
  g_test_message ("Confined connection saw NameOwnerChanged: \"%s\" owner "
                  "\"%s\" -> \"%s\"",
                  noc->name, noc->old_owner, noc->new_owner);
  g_queue_push_tail (&f->name_owner_changes, noc);

  if (g_strcmp0 (noc->name, f->confined_unique_names[1]) == 0)
    {
      if (noc->new_owner[0] != '\0')
        f->confined_1_name_owned = NAME_TRISTATE_OWNED;
      else
        f->confined_1_name_owned = NAME_TRISTATE_NOT_OWNED;
    }
}

static gboolean
try_request_name (GDBusConnection *connection,
                  const gchar *name,
                  guint32 *result,
                  GError **error)
{
  GVariant *reply;

  reply = g_dbus_connection_call_sync (
      connection, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
      "RequestName",
      g_variant_new ("(su)", name, DBUS_NAME_FLAG_DO_NOT_QUEUE),
      G_VARIANT_TYPE ("(u)"),
      G_DBUS_CALL_FLAGS_NONE,
      -1,
      NULL,
      error);

  if (reply != NULL)
    {
      g_variant_get (reply, "(u)", result);

      if (error != NULL)
        g_assert_null (*error);

      g_variant_unref (reply);
      return TRUE;
    }
  else if (error != NULL)
    {
      g_assert_nonnull (*error);
    }

  return FALSE;
}

static void
assert_request_name_succeeds (GDBusConnection *connection,
                              const gchar *name)
{
  GError *error = NULL;
  guint32 result;

  try_request_name (connection, name, &result, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (result, ==, DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
}

static gboolean
try_release_name (GDBusConnection *connection,
                  const gchar *name,
                  guint32 *result,
                  GError **error)
{
  GVariant *reply;

  reply = g_dbus_connection_call_sync (
      connection, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
      "ReleaseName",
      g_variant_new ("(s)", name),
      G_VARIANT_TYPE ("(u)"),
      G_DBUS_CALL_FLAGS_NONE,
      -1,
      NULL,
      error);

  if (reply != NULL)
    {
      g_variant_get (reply, "(u)", result);

      if (error != NULL)
        g_assert_null (*error);

      g_variant_unref (reply);
      return TRUE;
    }
  else if (error != NULL)
    {
      g_assert_nonnull (*error);
    }

  return FALSE;
}

static void
assert_release_name_succeeds (GDBusConnection *connection,
                              const gchar *name)
{
  GError *error = NULL;
  guint32 result;

  try_release_name (connection, name, &result, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (result, ==, DBUS_RELEASE_NAME_REPLY_RELEASED);
}

/*
 * Helper for Allow tests: Assert that GetNameOwner(), NameHasOwner()
 * and the given result of ListNames() agree.
 *
 * @names is really a (const gchar * const *) but it's passed via a
 * gconstpointer to avoid a lot of very ugly casts.
 */
static void
fixture_assert_name_visibility (Fixture *f,
                                const gchar *name,
                                gboolean is_visible,
                                gconstpointer names)
{
  GVariant *reply;
  gboolean b;

  g_test_message ("Checking that GetNameOwner, NameHasOwner and ListNames "
                  "all agree that the confined connection %s see \"%s\"",
                  is_visible ? "can" : "cannot", name);

  reply = g_dbus_connection_call_sync (f->confined_conns[0],
                                       DBUS_SERVICE_DBUS,
                                       DBUS_PATH_DBUS,
                                       DBUS_INTERFACE_DBUS,
                                       "GetNameOwner",
                                       g_variant_new ("(s)", name),
                                       G_VARIANT_TYPE ("(s)"),
                                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                       &f->error);

  if (is_visible)
    {
      const gchar *s;

      g_assert_no_error (f->error);
      g_assert_nonnull (reply);

      if (name[0] == ':')
        {
          g_variant_get (reply, "(&s)", &s);
          g_assert_cmpstr (name, ==, s);
        }

      g_clear_pointer (&reply, g_variant_unref);
    }
  else
    {
      g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER);
      g_clear_error (&f->error);
    }

  reply = g_dbus_connection_call_sync (f->confined_conns[0],
                                       DBUS_SERVICE_DBUS,
                                       DBUS_PATH_DBUS,
                                       DBUS_INTERFACE_DBUS,
                                       "NameHasOwner",
                                       g_variant_new ("(s)", name),
                                       G_VARIANT_TYPE ("(b)"),
                                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                       &f->error);

  g_assert_no_error (f->error);
  g_assert_nonnull (reply);
  g_variant_get (reply, "(b)", &b);
  g_assert_cmpint (is_visible, ==, b);

  g_assert_cmpint (is_visible, ==, g_strv_contains (names, name));
}

/*
 * Helper for Allow tests: Return a Unix fd list containing a
 * reference to /dev/null.
 */
static GUnixFDList *
new_unix_fd_list (void)
{
  gint fd;

  fd = g_open ("/dev/null", O_RDONLY, 0666);

  if (fd < 0)
    g_error ("cannot open /dev/null: %s", g_strerror (errno));

  return g_unix_fd_list_new_from_array (&fd, 1);
}

/*
 * Helper for Allow tests: Attach /dev/null to the given message.
 */
static void
message_set_body_to_unix_fd (GDBusMessage *message)
{
  GUnixFDList *fd_list = new_unix_fd_list ();

  g_dbus_message_set_body (message, g_variant_new ("(h)", 0));
  g_dbus_message_set_unix_fd_list (message, fd_list);
  g_clear_object (&fd_list);
}

/* A magic string that does not appear in any message that we expect to
 * be allowed through. We use this because otherwise, it's difficult
 * to tell the difference between various replies. When we add more
 * general message filtering, it will also provide an easy way to
 * assert that messages are blocked at the dbus-daemon and never arrive
 * at the addressed destination. */
#define UNDELIVERABLE_CONTENTS \
  "This content should not have been delivered"

/*
 * Helper for Allow tests, used to implement methods and detect
 * unsolicited replies. We do this in a filter instead of actually
 * exporting objects so that the test is free to call arbitrary
 * methods on the other connection.
 *
 * Note that this is invoked in the worker thread.
 */
static GDBusMessage *
allow_tests_message_filter (GDBusConnection *connection,
                            GDBusMessage *message,
                            gboolean incoming,
                            gpointer user_data)
{
  GDBusMessage *reply;
  GError *error = NULL;
  GVariant *body;

  /* We only care about incoming messages here; outgoing messages can
   * be ignored. */
  if (!incoming)
    return message;

  body = g_dbus_message_get_body (message);

  if (body != NULL &&
      g_variant_is_of_type (body, G_VARIANT_TYPE ("(s)")))
    {
      const gchar *s;

      g_variant_get (body, "(&s)", &s);

      if (g_strcmp0 (s, UNDELIVERABLE_CONTENTS) == 0)
        g_error ("Message with special marker should not have been received");
    }

  /* If no reply is expected, don't. */
  if (g_dbus_message_get_message_type (message) !=
      G_DBUS_MESSAGE_TYPE_METHOD_CALL ||
      (g_dbus_message_get_flags (message) &
       G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED) != 0)
    return message;

  /* Reply to method calls that expect a reply. */
  reply = g_dbus_message_new_method_reply (message);

  if (g_strcmp0 (g_dbus_message_get_member (message), "ReplyWithFd") == 0)
    {
      /* Implement the ReplyWithFd method (at all object paths, on all
       * interfaces) to reply with a fd. */
      message_set_body_to_unix_fd (reply);
    }
  else if (g_strcmp0 (g_dbus_message_get_member (message),
                      "RaiseUnknownMethod") == 0)
    {
      g_clear_object (&reply);
      reply = g_dbus_message_new_method_error (message,
          DBUS_ERROR_UNKNOWN_METHOD, "No such method");
    }
   else if (g_strcmp0 (g_dbus_message_get_interface (message),
                      DBUS_INTERFACE_PROPERTIES) == 0 &&
           g_strcmp0 (g_dbus_message_get_member (message), "GetAll") == 0)
    {
      /* Return a realistic type for GetAll. */
      g_dbus_message_set_body (reply, g_variant_new ("(a{sv})", NULL));
    }
  else if (g_strcmp0 (g_dbus_message_get_interface (message),
                      DBUS_INTERFACE_PROPERTIES) == 0 &&
           g_strcmp0 (g_dbus_message_get_member (message), "Get") == 0)
    {
      /* Return a realistic type for Get. */
      g_dbus_message_set_body (reply,
                               g_variant_new ("(v)",
                                              g_variant_new_uint32 (42)));
    }
  else
    {
      /* Implement all other methods to reply with nothing. */
    }

  g_dbus_connection_send_message (connection, reply,
                                  G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                  NULL, &error);
  g_clear_object (&reply);
  g_assert_no_error (error);

  /* Message has been handled */
  g_clear_object (&message);
  return NULL;
}

/*
 * Helper for Allow tests: GDBusSignalCallback that counts how many
 * times we got a particular signal.
 */
static void
count_signal_cb (GDBusConnection *connection,
                 const gchar *sender_name,
                 const gchar *object_path,
                 const gchar *interface_name,
                 const gchar *signal_name,
                 GVariant *parameters,
                 gpointer user_data)
{
  guint *counter = user_data;

  g_test_message ("Connection %p received %s.%s from %s:%s",
                  connection, interface_name, signal_name,
                  sender_name, object_path);

  *counter += 1;
}

static gboolean add_container_server (Fixture *f,
                                      GVariant *parameters);
#endif

/* Special bus names that are replaced by the appropriate unique name
 * if they appear in AllowMessage.argument or (where possible)
 * AllowRule.bus_name */
#define REPLACE_WITH_UNCONFINED_UNIQUE_NAME ":unconfined"
#define REPLACE_WITH_OBSERVER_UNIQUE_NAME ":observer"
#define REPLACE_WITH_CONFINED_UNIQUE_NAME ":confined"
#define REPLACE_WITH_CONFINED_1_UNIQUE_NAME ":confined[1]"

/*
 * Simple C representation of an Allow rule for use in static
 * const structs
 */
typedef struct
{
  guint flags;
  const char *bus_name;
  const char *object_path;
  const char *interface_and_maybe_member;
} AllowRule;

/*
 * Flags affecting messages that we should or shouldn't be able to send
 */
typedef enum
{
  /* If set, the signal or method call will contain a file descriptor,
   * which isn't always allowed */
  ALLOW_MESSAGE_FLAGS_SEND_FD = (1 << 0),

  /* If set, the method call's reply will contain a file descriptor.
   * Meaningless for signals. */
  ALLOW_MESSAGE_FLAGS_FD_IN_REPLY = (1 << 1),

  /* If set, the signal or method call will come from unconfined_conn
   * outside the container. If not set, the signal or method call will
   * come from confined_conns[0] inside the container. Either way, the
   * reply (if any) is expected to come from the destination, whatever
   * that happens to be. */
  ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE = (1 << 2),

  ALLOW_MESSAGE_FLAGS_NONE = 0
} AllowMessageFlags;

/*
 * The result of a method call that we should or shouldn't be able
 * to send
 */
typedef enum
{
  /* Method calls: If (flags & INITIATOR_OUTSIDE), unconfined_conn can
   * call a method on a confined connection. Otherwise, confined_conn
   * can call a method on some other connection. */

  /* Method should return some successful reply. We make no statement
   * about its contents. */
  METHOD_SUCCEEDS = 1,
  /* Method should return boolean 'true' */
  METHOD_RETURNS_TRUE,
  /* Method should return boolean 'false' */
  METHOD_RETURNS_FALSE,
  /* Method should raise AccessDenied */
  METHOD_RAISES_ACCESS_DENIED,
  /* Method should raise NameHasNoOwner */
  METHOD_RAISES_NAME_HAS_NO_OWNER,
  /* Method should raise UnixProcessIDUnknown or similar */
  METHOD_RAISES_CANNOT_INSPECT,
  /* Method should raise UnknownMethod */
  METHOD_RAISES_UNKNOWN_METHOD,
  /* Method should raise InvalidArgs */
  METHOD_RAISES_INVALID_ARGS,
  /* Method should either return some successful reply, or return an
   * error that is not AccessDenied. */
  METHOD_ALLOWS_ACCESS,

  /* Array terminator */
  METHOD_INVALID = 0
} AllowMethodCallResult;

/*
 * A method call that we should or shouldn't be able to send
 */
typedef struct
{
  AllowMethodCallResult result;
  /* The destination, or NULL if we are communicating with the dbus-daemon
   * as a peer */
  const char *bus_name;
  /* The destination object path */
  const char *object_path;
  /* The interface */
  const char *iface;
  /* The member name. Some member names are given special meaning
   * as a short-cut for defining test cases, for example AddMatch
   * gets a valid match rule. */
  const char *member;
  /* A string argument or NULL */
  const char *argument;
  AllowMessageFlags flags;
} AllowMethodCall;

/*
 * The result of a signal that we should or shouldn't be able to send
 */
typedef enum
{
  /* Signal should be delivered */
  SIGNAL_DELIVERED,
  /* Signal should not be delivered */
  SIGNAL_NOT_DELIVERED,

  /* Array terminator */
  SIGNAL_INVALID = 0
} AllowSignalResult;

/*
 * A signal that we should or shouldn't be able to send
 */
typedef struct
{
  AllowSignalResult result;
  /* The destination, or NULL for a broadcast signal */
  const char *bus_name;
  /* The source object path */
  const char *object_path;
  /* The interface */
  const char *iface;
  /* The member name. Some member names are given special meaning
   * as a short-cut for defining test cases, for example AddMatch
   * gets a valid match rule. */
  const char *member;
  /* A string argument or NULL */
  const char *argument;
  AllowMessageFlags flags;
} AllowSignal;

/*
 * Flags affecting an entire test-case
 */
typedef enum
{
  /* If set, the array of rules must be empty (the first one must have
   * flags == 0) and we will not set the Allow named-parameter at all */
  ALLOW_TEST_FLAGS_OMIT_ALLOW = (1 << 0),
  ALLOW_TEST_FLAGS_NONE = 0
} AllowTestFlags;

/*
 * A test-case for Allow rules.
 *
 * Arrays in this structure are of arbitrary length: any length that
 * is sufficient for allow_rules_tests will do. Always use G_N_ELEMENTS
 * when iterating over them.
 */
typedef struct
{
  const char *name;
  AllowTestFlags flags;
  /* Can be terminated early by an entry with flags 0 */
  const AllowRule rules[16];
  const char *own_name;
  /* Can be terminated early by an entry with type NULL */
  const char * const can_see_names[16];
  /* Can be terminated early by an entry with type NULL */
  const char * const cannot_see_names[16];
  /* Can be terminated early by an entry with result INVALID */
  const AllowMethodCall method_calls[64];
  /* Can be terminated early by an entry with result INVALID */
  const AllowSignal signals[64];
} AllowRulesTest;

static const AllowRulesTest allow_rules_tests[] =
{
  { /* Test-case: If the Allow parameter is omitted, the confined
     * connection can do most things. */
    "omit-allow", ALLOW_TEST_FLAGS_OMIT_ALLOW,

    { /* rules: no rules */
      { 0 }
    },
    /* own_name: We can (and will) own this name */
    "com.example.Confined",
    { /* can_see_names: We can see these names */
      "org.freedesktop.DBus",
      "com.example.Confined",
      "com.example.Observer",
      "com.example.SystemdActivatable1",
      "com.example.Unconfined",
      NULL
    },
    {
      /* cannot_see_names: We can't see these names */
      NULL
    },
    { /* method_calls: */

      /* We don't explicitly test Hello() here, but if it didn't work,
       * then the confined connection would fail to connect; so it must
       * work even when restricted. */

      /* We have to test whether we can see the unconfined connection
       * before it calls our methods or sends unicast signals to us,
       * because those actions implicitly add SEE access. */
      { METHOD_SUCCEEDS,
        DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
        "GetNameOwner", REPLACE_WITH_UNCONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_RETURNS_TRUE,
        DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
        "NameHasOwner", REPLACE_WITH_UNCONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* May call Peer methods on the dbus-daemon as our peer */
      { METHOD_SUCCEEDS,
        NULL, "/", DBUS_INTERFACE_PEER, "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* We can't rely on this succeeding in autobuilder environments
       * that might not have a machine ID, but if it fails, it should
       * be with FileNotFound */
      { METHOD_ALLOWS_ACCESS,
        NULL, "/", DBUS_INTERFACE_PEER, "GetMachineId", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* May call Peer methods on the bus driver */
      { METHOD_SUCCEEDS,
        DBUS_SERVICE_DBUS, "/", DBUS_INTERFACE_PEER, "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* As above, we can't rely on this succeeding */
      { METHOD_ALLOWS_ACCESS,
        DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_PEER,
        "GetMachineId", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_SUCCEEDS,
        DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_INTROSPECTABLE,
        "Introspect", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* May call unrestricted methods on the bus driver */
      { METHOD_SUCCEEDS,
        DBUS_SERVICE_DBUS, "/", DBUS_INTERFACE_DBUS,
        "GetId", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_SUCCEEDS,
        DBUS_SERVICE_DBUS, "/", DBUS_INTERFACE_DBUS,
        "AddMatch", "type='signal'",
        ALLOW_MESSAGE_FLAGS_NONE },

      /* Must not eavesdrop (even though it is otherwise unrestricted) */
      { METHOD_RAISES_ACCESS_DENIED,
        DBUS_SERVICE_DBUS, "/", DBUS_INTERFACE_DBUS,
        "AddMatch", "type='signal',eavesdrop='true'",
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_RAISES_ACCESS_DENIED,
        DBUS_SERVICE_DBUS, "/", DBUS_INTERFACE_DBUS,
        "AddMatch", "type='signal',eavesdrop=true",
        ALLOW_MESSAGE_FLAGS_NONE },

      /* May receive method calls from outside.
       * May send success or error replies to such method calls. */
      { METHOD_SUCCEEDS,
        REPLACE_WITH_CONFINED_UNIQUE_NAME, "/", DBUS_INTERFACE_PEER,
        "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE },
      { METHOD_RAISES_UNKNOWN_METHOD,
        REPLACE_WITH_CONFINED_UNIQUE_NAME, "/", DBUS_INTERFACE_PEER,
        "RaiseUnknownMethod", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE },

      /* Peers inside the container may communicate among themselves */
      { METHOD_SUCCEEDS,
        REPLACE_WITH_CONFINED_1_UNIQUE_NAME, "/",
        DBUS_INTERFACE_PEER, "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_NONE, },

      /* Must be able to inspect connections inside the container,
       * identified by their well-known names */
      { METHOD_SUCCEEDS,
        DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
        "GetConnectionCredentials", "com.example.Confined",
        ALLOW_MESSAGE_FLAGS_NONE },

      /* May send fds to dbus-daemon (we get a different error but
       * that's OK) */
      { METHOD_RAISES_INVALID_ARGS,
        DBUS_SERVICE_DBUS, "/", DBUS_INTERFACE_PEER, "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_SEND_FD },
      /* We can't test whether sending fds to dbus-daemon in a
       * signal is allowed (but it's academic, because it's going to
       * receive them whether it wants to or not) */

      /* May send fds to outside */
      { METHOD_SUCCEEDS,
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME, "/",
        "com.example.Foo", "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_SEND_FD },

      /* May receive fds from outside */
      { METHOD_SUCCEEDS,
        REPLACE_WITH_CONFINED_UNIQUE_NAME, "/",
        "com.example.Foo", "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE | ALLOW_MESSAGE_FLAGS_SEND_FD },

      /* May send fds to outside in replies */
      { METHOD_SUCCEEDS,
        REPLACE_WITH_CONFINED_UNIQUE_NAME, "/",
        "com.example.Test", "ReplyWithFd", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE | ALLOW_MESSAGE_FLAGS_FD_IN_REPLY },

      /* May receive fds from outside in replies */
      { METHOD_SUCCEEDS,
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME, "/",
        "com.example.Test", "ReplyWithFd", NULL,
        ALLOW_MESSAGE_FLAGS_FD_IN_REPLY },

      /* May send method calls to outside */
      { METHOD_SUCCEEDS,
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME, "/",
        DBUS_INTERFACE_PEER, "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_SUCCEEDS,
        "com.example.Unconfined", "/",
        DBUS_INTERFACE_PEER, "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* Must be able to request arbitrary well-known names */
      { METHOD_SUCCEEDS, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        DBUS_INTERFACE_DBUS, "RequestName", "com.example.Hello",
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_SUCCEEDS, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        DBUS_INTERFACE_DBUS, "ReleaseName", "com.example.Hello",
        ALLOW_MESSAGE_FLAGS_NONE },

      /* Must be able to inspect connections outside the container */
      { METHOD_SUCCEEDS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionCredentials",
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* We assume that if containers work, so does getpeereid() or
       * equivalent */
      { METHOD_SUCCEEDS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionUnixUser",
        REPLACE_WITH_OBSERVER_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_ALLOWS_ACCESS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionUnixProcessID",
        "com.example.Unconfined",
        ALLOW_MESSAGE_FLAGS_NONE },
      /* This is Solaris-specific so the method call will be allowed,
       * but fail, on all other platforms */
      { METHOD_ALLOWS_ACCESS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetAdtAuditSessionData",
        "com.example.Observer",
        ALLOW_MESSAGE_FLAGS_NONE },
      /* The dbus-daemon itself counts as being outside the container.
       * This will be allowed, but fail, on non-SELinux systems */
      { METHOD_ALLOWS_ACCESS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionSELinuxSecurityContext",
        DBUS_SERVICE_DBUS,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_ALLOWS_ACCESS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1, "GetConnectionInstance",
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* Must be able to inspect connections inside the container */
      { METHOD_SUCCEEDS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionCredentials",
        REPLACE_WITH_CONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* We assume that if containers work, so does getpeereid() or
       * equivalent */
      { METHOD_SUCCEEDS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionUnixUser",
        REPLACE_WITH_CONFINED_1_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* We can't assume that we have enough credentials-passing to
       * know the process ID */
      { METHOD_ALLOWS_ACCESS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionUnixProcessID",
        REPLACE_WITH_CONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* This is Solaris-specific so the method call will be allowed,
       * but fail, on all other platforms */
      { METHOD_ALLOWS_ACCESS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetAdtAuditSessionData",
        REPLACE_WITH_CONFINED_1_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* This will be allowed, but fail, on non-SELinux systems */
      { METHOD_ALLOWS_ACCESS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionSELinuxSecurityContext",
        REPLACE_WITH_CONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_SUCCEEDS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1, "GetConnectionInstance",
        REPLACE_WITH_CONFINED_1_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },

      { METHOD_INVALID }    /* sentinel */
    },
    { /* signals: */

      /* Peers inside the container may communicate among themselves */
      { SIGNAL_DELIVERED,
        REPLACE_WITH_CONFINED_1_UNIQUE_NAME, "/",
        "com.example.Foo", "UnicastSignal", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* May receive unicast signals from outside */
      { SIGNAL_DELIVERED,
        REPLACE_WITH_CONFINED_UNIQUE_NAME, "/",
        "com.example.Foo", "UnicastSignal", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE },

      /* May send fds to outside */
      { SIGNAL_DELIVERED,
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME, "/",
        "com.example.Foo", "UnicastSignal", NULL,
        ALLOW_MESSAGE_FLAGS_SEND_FD },

      /* May receive fds from outside */
      { SIGNAL_DELIVERED,
        REPLACE_WITH_CONFINED_UNIQUE_NAME, "/",
        "com.example.Foo", "UnicastSignal", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE | ALLOW_MESSAGE_FLAGS_SEND_FD },

      /* May receive broadcasts from outside */
      { SIGNAL_DELIVERED,
        NULL, "/", "com.example.Foo", "BroadcastSignal", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE },

      /* May send unicast signals to outside */
      { SIGNAL_DELIVERED,
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME, "/",
        "com.example.Foo", "UnicastSignal", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },

      { SIGNAL_INVALID }    /* sentinel */
    }
  },

  { /* Test-case: If the Allow parameter is present but empty, the
     * confined connection cannot do most things. */
    "empty-allow", ALLOW_TEST_FLAGS_NONE,
    { /* rules: no rules */
      { 0 }
    },
    /* own_name: We will not be allowed to own a name when that
     * restriction is implemented, so don't try */
    NULL,
    { /* can_see_names: We can see these names */
      "org.freedesktop.DBus",
      NULL
    },
    { /* cannot_see_names: We can't see these names (even after
       * com.example.Unconfined calls a method on us, which will
       * eventually allow us to see its unique name) */
      "com.example.Confined",
      "com.example.Observer",
      "com.example.SystemdActivatable1",
      "com.example.Unconfined",
      NULL
    },
    { /* method_calls: */

      /* We don't explicitly test Hello() here, but if it didn't work,
       * then the confined connection would fail to connect; so it must
       * work even when restricted. */

      /* We have to test whether we can see the unconfined connection
       * before it calls our methods or sends unicast signals to us,
       * because those actions implicitly add SEE access. */
      { METHOD_RAISES_NAME_HAS_NO_OWNER,
        DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
        "GetNameOwner", REPLACE_WITH_UNCONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_RETURNS_FALSE,
        DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
        "NameHasOwner", REPLACE_WITH_UNCONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* Trying to inspect a connection we can't see also yields
       * NAME_HAS_NO_OWNER */
      { METHOD_RAISES_NAME_HAS_NO_OWNER, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionCredentials",
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* May call Peer methods on the dbus-daemon as our peer */
      { METHOD_SUCCEEDS,
        NULL, "/", DBUS_INTERFACE_PEER, "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* As above, we can't rely on this succeeding */
      { METHOD_ALLOWS_ACCESS,
        NULL, "/", DBUS_INTERFACE_PEER, "GetMachineId", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* May call Peer methods on the bus driver */
      { METHOD_SUCCEEDS,
        DBUS_SERVICE_DBUS, "/", DBUS_INTERFACE_PEER, "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* As above, we can't rely on this succeeding */
      { METHOD_ALLOWS_ACCESS,
        DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_PEER,
        "GetMachineId", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_SUCCEEDS,
        DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_INTROSPECTABLE,
        "Introspect", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* May call unrestricted methods on the bus driver */
      { METHOD_SUCCEEDS,
        DBUS_SERVICE_DBUS, "/", DBUS_INTERFACE_DBUS,
        "GetId", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_SUCCEEDS,
        DBUS_SERVICE_DBUS, "/", DBUS_INTERFACE_DBUS,
        "AddMatch", "type='signal'",
        ALLOW_MESSAGE_FLAGS_NONE },

      /* Must not eavesdrop */
      { METHOD_RAISES_ACCESS_DENIED,
        DBUS_SERVICE_DBUS, "/", DBUS_INTERFACE_DBUS,
        "AddMatch", "type='signal',eavesdrop='true'",
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_RAISES_ACCESS_DENIED,
        DBUS_SERVICE_DBUS, "/", DBUS_INTERFACE_DBUS,
        "AddMatch", "type='signal',eavesdrop=true",
        ALLOW_MESSAGE_FLAGS_NONE },

      /* May receive method calls from outside, as long as there are no
       * Unix fds attached.
       * May send success or error replies to such method calls. */
      { METHOD_SUCCEEDS,
        REPLACE_WITH_CONFINED_UNIQUE_NAME, "/", DBUS_INTERFACE_PEER,
        "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE },
      { METHOD_RAISES_UNKNOWN_METHOD,
        REPLACE_WITH_CONFINED_UNIQUE_NAME, "/", DBUS_INTERFACE_PEER,
        "RaiseUnknownMethod", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE },

      /* Peers inside the container may communicate among themselves */
      { METHOD_SUCCEEDS,
        REPLACE_WITH_CONFINED_1_UNIQUE_NAME, "/",
        DBUS_INTERFACE_PEER, "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_NONE, },

      /* Must not send fds to dbus-daemon */
      { METHOD_RAISES_ACCESS_DENIED,
        DBUS_SERVICE_DBUS, "/", DBUS_INTERFACE_PEER, "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_SEND_FD },
      /* We can't test whether sending fds to dbus-daemon in a
       * signal is allowed (but it's academic, because it's going to
       * receive them whether it wants to or not) */

      /* Must not send fds to outside */
      { METHOD_RAISES_ACCESS_DENIED,
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME, "/",
        "com.example.Foo", "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_SEND_FD },

      /* Must not receive fds from outside */
      { METHOD_RAISES_ACCESS_DENIED,
        REPLACE_WITH_CONFINED_UNIQUE_NAME, "/",
        "com.example.Foo", "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE | ALLOW_MESSAGE_FLAGS_SEND_FD },

      /* Must not send fds to outside in replies */
      { METHOD_RAISES_ACCESS_DENIED,
        REPLACE_WITH_CONFINED_UNIQUE_NAME, "/",
        "com.example.Test", "ReplyWithFd", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE | ALLOW_MESSAGE_FLAGS_FD_IN_REPLY },

      /* Must not receive fds from outside in replies */
      { METHOD_RAISES_ACCESS_DENIED,
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME, "/",
        "com.example.Test", "ReplyWithFd", NULL,
        ALLOW_MESSAGE_FLAGS_FD_IN_REPLY },

      /* Must not send method calls to outside */
      { METHOD_RAISES_ACCESS_DENIED,
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME, "/",
        DBUS_INTERFACE_PEER, "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_RAISES_ACCESS_DENIED,
        "com.example.Unconfined", "/",
        DBUS_INTERFACE_PEER, "Ping", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* Must not be able to request a well-known name */
      { METHOD_RAISES_ACCESS_DENIED, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        DBUS_INTERFACE_DBUS, "RequestName", "com.example.Confined",
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_RAISES_ACCESS_DENIED, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        DBUS_INTERFACE_DBUS, "RequestName", "com.example.Observer",
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_RAISES_ACCESS_DENIED, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        DBUS_INTERFACE_DBUS, "RequestName", "com.example.SystemdActivatable1",
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_RAISES_ACCESS_DENIED, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        DBUS_INTERFACE_DBUS, "RequestName", "com.example.Unconfined",
        ALLOW_MESSAGE_FLAGS_NONE },

      /* Must not release a well-known name (if we could, we could use the
       * result as an oracle to see whether it's owned) */
      { METHOD_RAISES_ACCESS_DENIED, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        DBUS_INTERFACE_DBUS, "ReleaseName", "com.example.Unconfined",
        ALLOW_MESSAGE_FLAGS_NONE },

      /* Must not become a monitor */
      { METHOD_RAISES_ACCESS_DENIED, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        DBUS_INTERFACE_MONITORING, "BecomeMonitor", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* Must not manipulate activation environment */
      { METHOD_RAISES_ACCESS_DENIED, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        DBUS_INTERFACE_DBUS, "UpdateActivationEnvironment", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* Must not manipulate Verbose/Stats (if supported) */
      { METHOD_RAISES_ACCESS_DENIED, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        DBUS_INTERFACE_VERBOSE, "EnableVerbose", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_RAISES_ACCESS_DENIED, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        BUS_INTERFACE_STATS, "GetStats", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* Must not be able to inspect connections outside the container */
#if 0
      /* TODO: We should get ACCESS_DENIED for this one, but we currently
       * get NAME_HAS_NO_OWNER because implicit SEE access for unique
       * names that have communicated with the container is not yet
       * implemented */
      { METHOD_RAISES_ACCESS_DENIED, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionCredentials",
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
#endif
      { METHOD_RAISES_NAME_HAS_NO_OWNER, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionUnixUser",
        REPLACE_WITH_OBSERVER_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_RAISES_NAME_HAS_NO_OWNER, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionUnixProcessID",
        "com.example.Unconfined",
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_RAISES_NAME_HAS_NO_OWNER, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetAdtAuditSessionData",
        "com.example.Observer",
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_RAISES_NAME_HAS_NO_OWNER, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1, "GetConnectionInstance",
        REPLACE_WITH_OBSERVER_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* The dbus-daemon itself counts as being outside the container */
      { METHOD_RAISES_ACCESS_DENIED, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionSELinuxSecurityContext",
        DBUS_SERVICE_DBUS,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* Must be able to inspect connections inside the container */
      { METHOD_SUCCEEDS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionCredentials",
        REPLACE_WITH_CONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* We assume that if containers work, so does getpeereid() or
       * equivalent */
      { METHOD_SUCCEEDS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionUnixUser",
        REPLACE_WITH_CONFINED_1_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* We can't assume that we have enough credentials-passing to
       * know the process ID */
      { METHOD_ALLOWS_ACCESS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionUnixProcessID",
        REPLACE_WITH_CONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* This is Solaris-specific so the method call will be allowed,
       * but fail, on all other platforms */
      { METHOD_ALLOWS_ACCESS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetAdtAuditSessionData",
        REPLACE_WITH_CONFINED_1_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      /* This will be allowed, but fail, on non-SELinux systems */
      { METHOD_ALLOWS_ACCESS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionSELinuxSecurityContext",
        REPLACE_WITH_CONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_SUCCEEDS, DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1, "GetConnectionInstance",
        REPLACE_WITH_CONFINED_1_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },

#if 0
      /* TODO: implicit SEE access for unique
       * names that have communicated with the container is not yet
       * implemented */
      /* After the unconfined connection has contacted us, we can SEE it. */
      { METHOD_SUCCEEDS, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        DBUS_INTERFACE_DBUS, "GetNameOwner",
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
      { METHOD_RETURNS_TRUE, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        DBUS_INTERFACE_DBUS, "NameHasOwner",
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME,
        ALLOW_MESSAGE_FLAGS_NONE },
#endif

      { METHOD_INVALID }     /* sentinel */
    },
    { /* signals: */

      /* Peers inside the container may communicate among themselves */
      { SIGNAL_DELIVERED,
        REPLACE_WITH_CONFINED_1_UNIQUE_NAME, "/",
        "com.example.Foo", "UnicastSignal", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },

      /* May receive unicast signals from outside */
      { SIGNAL_DELIVERED,
        REPLACE_WITH_CONFINED_UNIQUE_NAME, "/",
        "com.example.Foo", "UnicastSignal", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE },

      /* We can't test whether sending fds to dbus-daemon in a
       * signal is allowed (but it's academic, because it's going to
       * receive them whether it wants to or not) */

      /* Must not send fds to outside */
      { SIGNAL_NOT_DELIVERED,
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME, "/",
        "com.example.Foo", "UnicastSignal", NULL,
        ALLOW_MESSAGE_FLAGS_SEND_FD },

      /* Must not receive fds from outside */
      { SIGNAL_NOT_DELIVERED,
        REPLACE_WITH_CONFINED_UNIQUE_NAME, "/",
        "com.example.Foo", "UnicastSignal", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE | ALLOW_MESSAGE_FLAGS_SEND_FD },

      /* Must not receive broadcasts from outside */
      { SIGNAL_NOT_DELIVERED,
        NULL, "/", "com.example.Foo", "BroadcastSignal", NULL,
        ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE },

      /* Must not send unicast signals to outside */
      { SIGNAL_NOT_DELIVERED,
        REPLACE_WITH_UNCONFINED_UNIQUE_NAME, "/",
        "com.example.Foo", "UnicastSignal", NULL,
        ALLOW_MESSAGE_FLAGS_NONE },

      { SIGNAL_INVALID }    /* sentinel */
    }
  }
};

#ifdef HAVE_CONTAINERS_TEST
/*
 * Return TRUE if the test says @name should be visible to the confined
 * connections, or FALSE if either it should not be visible or there is
 * no guarantee either way.
 */
static gboolean
allow_rules_test_can_see (const AllowRulesTest *test,
                          const char *name)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (test->can_see_names); i++)
    {
      if (test->can_see_names[i] == NULL)
        break;

      if (g_strcmp0 (name, test->can_see_names[i]) == 0)
        return TRUE;
    }

  return FALSE;
}

/*
 * Return TRUE if the test says @name should be not visible to the
 * confined connections, or FALSE if either it should be visible or
 * there is no guarantee either way.
 */
static gboolean
allow_rules_test_cannot_see (const AllowRulesTest *test,
                             const char *name)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (test->cannot_see_names); i++)
    {
      if (test->cannot_see_names[i] == NULL)
        break;

      if (g_strcmp0 (name, test->cannot_see_names[i]) == 0)
        return TRUE;
    }

  return FALSE;
}
#endif

static void
setup (Fixture *f,
       gconstpointer context)
{
  const Config *config = context;

  if (config == NULL)
    config = &default_config;

  f->confined_1_name_owned = NAME_TRISTATE_MAYBE_OWNED;
  g_queue_init (&f->name_owner_changes);
  f->ctx = test_main_context_get ();

  f->bus_address = test_get_dbus_daemon (config->config_file, TEST_USER_ME,
                                         NULL, &f->daemon_pid);

  if (f->bus_address == NULL)
    {
      f->skip = TRUE;
      return;
    }

  f->unconfined_conn = g_dbus_connection_new_for_address_sync (f->bus_address,
      (G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION |
       G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT),
      NULL, NULL, &f->error);
  g_assert_no_error (f->error);
  f->unconfined_unique_name = g_strdup (
      g_dbus_connection_get_unique_name (f->unconfined_conn));
  g_test_message ("Unconfined connection: \"%s\"",
                  f->unconfined_unique_name);

  f->observer_conn = g_dbus_connection_new_for_address_sync (f->bus_address,
      (G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION |
       G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT),
      NULL, NULL, &f->error);
  g_assert_no_error (f->error);
  f->observer_unique_name = g_strdup (
      g_dbus_connection_get_unique_name (f->observer_conn));
  g_test_message ("Unconfined observer connection: \"%s\"",
                  f->observer_unique_name);

  f->containers_removed = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, NULL);
  f->removed_sub = g_dbus_connection_signal_subscribe (f->observer_conn,
                                                       DBUS_SERVICE_DBUS,
                                                       DBUS_INTERFACE_CONTAINERS1,
                                                       "InstanceRemoved",
                                                       DBUS_PATH_DBUS, NULL,
                                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                                       instance_removed_cb,
                                                       f, NULL);

  /* We have to use libdbus for new header fields, because GDBus doesn't
   * yet have API for that. */
  f->libdbus_observer = test_connect_to_bus (f->ctx, f->bus_address);
  dbus_bus_add_match (f->libdbus_observer,
                      "interface='com.example.Shouting'", NULL);

  if (!dbus_connection_add_filter (f->libdbus_observer, observe_shouting_cb, f,
                                   NULL))
    g_error ("OOM");

  f->observer_unique_name_owned = NAME_TRISTATE_MAYBE_OWNED;
  f->observer_unique_name_watch = g_bus_watch_name_on_connection (
      f->unconfined_conn, f->observer_unique_name,
      G_BUS_NAME_WATCHER_FLAGS_NONE,
      observer_appeared_cb, observer_vanished_cb,
      &f->observer_unique_name_owned,
      NULL);
  f->observer_well_known_name_owned = NAME_TRISTATE_MAYBE_OWNED;
  f->observer_well_known_name_watch = g_bus_watch_name_on_connection (
      f->unconfined_conn, "com.example.Observer",
      G_BUS_NAME_WATCHER_FLAGS_NONE,
      observer_appeared_cb, observer_vanished_cb,
      &f->observer_well_known_name_owned,
      NULL);
}

#ifdef HAVE_CONTAINERS_TEST
/* Names that are made activatable by systemd-activation.conf. This
 * list does not have to be exhaustive (and in particular we skip
 * org.freedesktop.systemd1 here because that's really just a
 * workaround), it just has to be enough for testing. */
static const char * const activatable_names[] =
{
  "com.example.ReceiveDenied",
  "com.example.ReceiveDeniedByAppArmorLabel",
  "com.example.SendDenied",
  "com.example.SendDeniedByAppArmorLabel",
  "com.example.SendDeniedByAppArmorName",
  "com.example.SendDeniedByNonexistentAppArmorLabel",
  "com.example.SystemdActivatable1",
  "com.example.SystemdActivatable2",
  "com.example.SystemdActivatable3",
  /* For some reason this counts as activatable too */
  "org.freedesktop.DBus"
};
#endif

/*
 * A Config with some activatable services, because test_allow() needs
 * to test ListActivatableNames, and to do that we need to be able to
 * predict what's in it.
 */
static const Config config_with_activatables =
{
  "valid-config-files/systemd-activation.conf",
  0 /* not relevant for this test */
};

static void
set_up_allow_test (Fixture *f,
                   gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  const AllowRulesTest *test = context;
  GVariantDict named_argument_builder;
  GVariantBuilder allow_builder;
  GVariant *parameters = NULL;
  guint i;
#endif

  /* Normally setup() assumes context is a const Config *, but
   * test_allow() needs to use context for the const AllowRulesTest *. */
  setup (f, &config_with_activatables);

#ifdef HAVE_CONTAINERS_TEST
  if (f->skip)
    return;

  g_variant_dict_init (&named_argument_builder, NULL);
  g_variant_builder_init (&allow_builder, G_VARIANT_TYPE ("a(usos)"));

  for (i = 0; i < G_N_ELEMENTS (test->rules); i++)
    {
      const AllowRule *rule = &test->rules[i];
      const char *bus_name = rule->bus_name;

      if (rule->flags == 0)
        break;

      /* We can't use the confined connections' unique names in Allow
       * rules, because we can't create those connections until the
       * container server is already up. */
      if (g_strcmp0 (bus_name, REPLACE_WITH_CONFINED_UNIQUE_NAME) == 0 ||
          g_strcmp0 (bus_name, REPLACE_WITH_CONFINED_1_UNIQUE_NAME) == 0)
        g_error ("test violates causality");
      else if (g_strcmp0 (bus_name, REPLACE_WITH_UNCONFINED_UNIQUE_NAME) == 0)
        bus_name = f->unconfined_unique_name;
      else if (g_strcmp0 (bus_name, REPLACE_WITH_OBSERVER_UNIQUE_NAME) == 0)
        bus_name = f->observer_unique_name;
      else
        g_assert (bus_name == NULL || bus_name[0] != ':');

      g_test_message ("Allow[%u]: flags=%x name=\"%s\" path=\"%s\" "
                      "interface (and member?)=\"%s\"",
                      i, rule->flags, bus_name, rule->object_path,
                      rule->interface_and_maybe_member);

      g_variant_builder_add (&allow_builder, "(usos)",
                             rule->flags, bus_name,
                             rule->object_path,
                             rule->interface_and_maybe_member);
    }

  if (test->flags & ALLOW_TEST_FLAGS_OMIT_ALLOW)
    g_assert (i == 0);    /* having any rules would make no sense */
  else
    g_variant_dict_insert (&named_argument_builder,
                           "Allow", "@a(usos)",
                           g_variant_builder_end (&allow_builder));

  parameters = g_variant_new ("(ssa{sv}@a{sv})",
                              "com.example.NotFlatpak",
                              "Confined",
                              NULL,
                              g_variant_dict_end (&named_argument_builder));

  if (!add_container_server (f, g_steal_pointer (&parameters)))
    return;

  for (i = 0; i < G_N_ELEMENTS (f->confined_conns); i++)
    {
      fixture_connect_confined (f, i);

      if (i == 0)
        {
          /* Watch for NameOwnerChanged on the first confined connection
           * before we let the second one connect. We'll use this later. */
          f->confined_0_noc_sub = g_dbus_connection_signal_subscribe (
              f->confined_conns[0], DBUS_SERVICE_DBUS, DBUS_INTERFACE_DBUS,
              "NameOwnerChanged", DBUS_PATH_DBUS, NULL,
              G_DBUS_SIGNAL_FLAGS_NONE,
              confined_0_name_owner_changed_cb, f, NULL);
        }

      if (i == 1 && test->own_name != NULL)
        {
          /* Give the second confined connection a well-known name
           * if necessary/possible, so we can test what happens when it
           * has one */
          assert_request_name_succeeds (f->confined_conns[1], test->own_name);
          g_test_message ("Confined connection %u: \"%s\" owns \"%s\"",
              i, g_dbus_connection_get_unique_name (f->confined_conns[i]),
              test->own_name);
        }
      else
        {
          g_test_message ("Confined connection %u: \"%s\"",
              i, f->confined_unique_names[i]);
        }
    }

  /* Give the unconfined connections well-known names so we can refer
   * to them later. We do this after connecting the confined connections
   * so that they will see the resulting NameOwnerChanged messages,
   * if allowed to do so. */
  assert_request_name_succeeds (f->unconfined_conn, "com.example.Unconfined");
  assert_request_name_succeeds (f->observer_conn, "com.example.Observer");

  /* Implement various method calls on the connections we will
   * use as possible method call destinations */
  f->unconfined_filter = g_dbus_connection_add_filter (
      f->unconfined_conn, allow_tests_message_filter, NULL, NULL);
  f->observer_filter = g_dbus_connection_add_filter (
      f->observer_conn, allow_tests_message_filter, NULL, NULL);

  for (i = 0; i < G_N_ELEMENTS (f->confined_conns); i++)
    f->confined_filters[i] = g_dbus_connection_add_filter (
        f->confined_conns[i], allow_tests_message_filter, NULL, NULL);
#endif
}

/*
 * Assert that Get(SupportedArguments) contains what we expect it to.
 */
static void
test_get_supported_arguments (Fixture *f,
                              gconstpointer context)
{
  GVariant *v;
#ifdef DBUS_ENABLE_CONTAINERS
  const gchar **args;
  gsize len;
#endif

  if (f->skip)
    return;

  f->proxy = g_dbus_proxy_new_sync (f->unconfined_conn, G_DBUS_PROXY_FLAGS_NONE,
                                    NULL, DBUS_SERVICE_DBUS,
                                    DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1,
                                    NULL, &f->error);

  /* This one is DBUS_ENABLE_CONTAINERS rather than HAVE_CONTAINERS_TEST
   * because we can still test whether the interface appears or not, even
   * if we were not able to detect gio-unix-2.0 */
#ifdef DBUS_ENABLE_CONTAINERS
  g_assert_no_error (f->error);

  v = g_dbus_proxy_get_cached_property (f->proxy, "SupportedArguments");
  g_assert_cmpstr (g_variant_get_type_string (v), ==, "as");
  args = g_variant_get_strv (v, &len);

  /* No arguments are defined yet */
  g_assert_cmpuint (len, ==, 0);

  g_free (args);
  g_variant_unref (v);
#else /* !DBUS_ENABLE_CONTAINERS */
  g_assert_no_error (f->error);
  v = g_dbus_proxy_get_cached_property (f->proxy, "SupportedArguments");
  g_assert_null (v);
#endif /* !DBUS_ENABLE_CONTAINERS */
}

#ifdef HAVE_CONTAINERS_TEST
/*
 * Try to make an AddServer call that usually succeeds, but may fail and
 * be skipped if we are running as root and this version of dbus has not
 * been fully installed. Return TRUE if we can continue.
 *
 * parameters is sunk if it is a floating reference.
 */
static gboolean
add_container_server (Fixture *f,
                      GVariant *parameters)
{
  GVariant *tuple;
  GStatBuf stat_buf;

  f->proxy = g_dbus_proxy_new_sync (f->unconfined_conn,
                                    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                    NULL, DBUS_SERVICE_DBUS,
                                    DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1,
                                    NULL, &f->error);
  g_assert_no_error (f->error);

  g_test_message ("Calling AddServer...");
  tuple = g_dbus_proxy_call_sync (f->proxy, "AddServer", parameters,
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, &f->error);

  /* For root, the sockets go in /run/dbus/containers, which we rely on
   * system infrastructure to create; so it's OK for AddServer to fail
   * when uninstalled, although not OK if it fails as an installed-test. */
  if (f->error != NULL &&
      _dbus_getuid () == 0 &&
      _dbus_getenv ("DBUS_TEST_UNINSTALLED") != NULL)
    {
      g_test_message ("AddServer: %s", f->error->message);
      g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_FILE_NOT_FOUND);
      g_test_skip ("AddServer failed, probably because this dbus "
                   "version is not fully installed");
      return FALSE;
    }

  g_assert_no_error (f->error);
  g_assert_nonnull (tuple);

  g_assert_cmpstr (g_variant_get_type_string (tuple), ==, "(oays)");
  g_variant_get (tuple, "(o^ays)", &f->instance_path, &f->socket_path,
                 &f->socket_dbus_address);
  g_assert_true (g_str_has_prefix (f->socket_dbus_address, "unix:"));
  g_assert_null (strchr (f->socket_dbus_address, ';'));
  g_assert_null (strchr (f->socket_dbus_address + strlen ("unix:"), ':'));
  g_clear_pointer (&tuple, g_variant_unref);

  g_assert_nonnull (f->instance_path);
  g_assert_true (g_variant_is_object_path (f->instance_path));
  g_assert_nonnull (f->socket_path);
  g_assert_true (g_path_is_absolute (f->socket_path));
  g_assert_nonnull (f->socket_dbus_address);
  g_assert_cmpstr (g_stat (f->socket_path, &stat_buf) == 0 ? NULL :
                   g_strerror (errno), ==, NULL);
  g_assert_cmpuint ((stat_buf.st_mode & S_IFMT), ==, S_IFSOCK);
  return TRUE;
}
#endif

/*
 * Assert that a simple AddServer() call succeeds and has the behaviour
 * we expect (we can connect a confined connection to it, the confined
 * connection can talk to the dbus-daemon and to an unconfined connection,
 * and the socket gets cleaned up when the dbus-daemon terminates).
 *
 * This also tests simple cases for metadata.
 */
static void
test_basic (Fixture *f,
            gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  GVariant *asv;
  GVariant *creator;
  GVariant *parameters;
  GVariantDict dict;
  const gchar *path_from_query;
  const gchar *name;
  const gchar *name_owner;
  const gchar *type;
  guint32 uid;
  GStatBuf stat_buf;
  GVariant *tuple;
  DBusMessage *libdbus_message = NULL;
  DBusMessage *libdbus_reply = NULL;
  DBusError libdbus_error = DBUS_ERROR_INIT;

  if (f->skip)
    return;

  parameters = g_variant_new ("(ssa{sv}a{sv})",
                              "com.example.NotFlatpak",
                              "sample-app",
                              NULL, /* no metadata */
                              NULL); /* no named arguments */
  if (!add_container_server (f, g_steal_pointer (&parameters)))
    return;

  fixture_connect_confined (f, 0);

  g_test_message ("Making a method call from confined app...");
  tuple = g_dbus_connection_call_sync (f->confined_conns[0], DBUS_SERVICE_DBUS,
                                       DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
                                       "GetNameOwner",
                                       g_variant_new ("(s)", DBUS_SERVICE_DBUS),
                                       G_VARIANT_TYPE ("(s)"),
                                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                       &f->error);
  g_assert_no_error (f->error);
  g_assert_nonnull (tuple);
  g_assert_cmpstr (g_variant_get_type_string (tuple), ==, "(s)");
  g_variant_get (tuple, "(&s)", &name_owner);
  g_assert_cmpstr (name_owner, ==, DBUS_SERVICE_DBUS);
  g_clear_pointer (&tuple, g_variant_unref);

  g_test_message ("Making a method call from confined app to unconfined...");
  tuple = g_dbus_connection_call_sync (f->confined_conns[0],
                                       f->unconfined_unique_name,
                                       "/", DBUS_INTERFACE_PEER,
                                       "Ping",
                                       NULL, G_VARIANT_TYPE_UNIT,
                                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                       &f->error);
  g_assert_no_error (f->error);
  g_assert_nonnull (tuple);
  g_assert_cmpstr (g_variant_get_type_string (tuple), ==, "()");
  g_clear_pointer (&tuple, g_variant_unref);

  g_test_message ("Receiving signals without requesting extra headers");
  g_dbus_connection_emit_signal (f->confined_conns[0], NULL, "/",
                                 "com.example.Shouting", "Shouted",
                                 NULL, NULL);

  while (f->latest_shout == NULL)
    iterate_both_main_loops (f->ctx);

  g_assert_cmpstr (dbus_message_get_container_instance (f->latest_shout), ==,
                   NULL);
  dbus_clear_message (&f->latest_shout);

  g_dbus_connection_emit_signal (f->unconfined_conn, NULL, "/",
                                 "com.example.Shouting", "Shouted",
                                 NULL, NULL);

  while (f->latest_shout == NULL)
    iterate_both_main_loops (f->ctx);

  g_assert_cmpstr (dbus_message_get_container_instance (f->latest_shout), ==,
                   NULL);
  dbus_clear_message (&f->latest_shout);

  g_test_message ("Receiving signals after requesting extra headers");

  libdbus_message = dbus_message_new_method_call (DBUS_SERVICE_DBUS,
                                                  DBUS_PATH_DBUS,
                                                  DBUS_INTERFACE_CONTAINERS1,
                                                  "RequestHeader");
  libdbus_reply = test_main_context_call_and_wait (f->ctx,
                                                   f->libdbus_observer,
                                                   libdbus_message,
                                                   DBUS_TIMEOUT_USE_DEFAULT);

  if (dbus_set_error_from_message (&libdbus_error, libdbus_reply))
    g_error ("%s", libdbus_error.message);

  dbus_clear_message (&libdbus_message);
  dbus_clear_message (&libdbus_reply);

  g_dbus_connection_emit_signal (f->confined_conns[0], NULL, "/",
                                 "com.example.Shouting", "Shouted",
                                 NULL, NULL);

  while (f->latest_shout == NULL)
    iterate_both_main_loops (f->ctx);

  g_assert_cmpstr (dbus_message_get_container_instance (f->latest_shout), ==,
                   f->instance_path);
  dbus_clear_message (&f->latest_shout);

  g_dbus_connection_emit_signal (f->unconfined_conn, NULL, "/",
                                 "com.example.Shouting", "Shouted",
                                 NULL, NULL);

  while (f->latest_shout == NULL)
    iterate_both_main_loops (f->ctx);

  g_assert_cmpstr (dbus_message_get_container_instance (f->latest_shout), ==,
                   "/");
  dbus_clear_message (&f->latest_shout);

  g_test_message ("Checking that confined app is not considered privileged...");
  tuple = g_dbus_connection_call_sync (f->confined_conns[0], DBUS_SERVICE_DBUS,
                                       DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
                                       "UpdateActivationEnvironment",
                                       g_variant_new ("(a{ss})", NULL),
                                       G_VARIANT_TYPE_UNIT,
                                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                       &f->error);
  g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED);
  g_test_message ("Access denied as expected: %s", f->error->message);
  g_clear_error (&f->error);
  g_assert_null (tuple);

  g_test_message ("Inspecting connection container info");
  f->confined_unique_names[0] = g_strdup (
      g_dbus_connection_get_unique_name (f->confined_conns[0]));
  tuple = g_dbus_proxy_call_sync (f->proxy, "GetConnectionInstance",
                                  g_variant_new ("(s)",
                                                 f->confined_unique_names[0]),
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, &f->error);
  g_assert_no_error (f->error);
  g_assert_nonnull (tuple);
  g_assert_cmpstr (g_variant_get_type_string (tuple), ==, "(oa{sv}ssa{sv})");
  g_variant_get (tuple, "(&o@a{sv}&s&s@a{sv})",
                 &path_from_query, &creator, &type, &name, &asv);
  g_assert_cmpstr (path_from_query, ==, f->instance_path);
  g_variant_dict_init (&dict, creator);
  g_assert_true (g_variant_dict_lookup (&dict, "UnixUserID", "u", &uid));
  g_assert_cmpuint (uid, ==, _dbus_getuid ());
  g_variant_dict_clear (&dict);
  g_assert_cmpstr (type, ==, "com.example.NotFlatpak");
  g_assert_cmpstr (name, ==, "sample-app");
  /* Trivial case: the metadata a{sv} is empty */
  g_assert_cmpuint (g_variant_n_children (asv), ==, 0);
  g_clear_pointer (&asv, g_variant_unref);
  g_clear_pointer (&creator, g_variant_unref);
  g_clear_pointer (&tuple, g_variant_unref);

  g_test_message ("Inspecting container instance info");
  tuple = g_dbus_proxy_call_sync (f->proxy, "GetInstanceInfo",
                                  g_variant_new ("(o)", f->instance_path),
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, &f->error);
  g_assert_no_error (f->error);
  g_assert_nonnull (tuple);
  g_assert_cmpstr (g_variant_get_type_string (tuple), ==, "(a{sv}ssa{sv})");
  g_variant_get (tuple, "(@a{sv}&s&s@a{sv})", &creator, &type, &name, &asv);
  g_variant_dict_init (&dict, creator);
  g_assert_true (g_variant_dict_lookup (&dict, "UnixUserID", "u", &uid));
  g_assert_cmpuint (uid, ==, _dbus_getuid ());
  g_variant_dict_clear (&dict);
  g_assert_cmpstr (type, ==, "com.example.NotFlatpak");
  g_assert_cmpstr (name, ==, "sample-app");
  /* Trivial case: the metadata a{sv} is empty */
  g_assert_cmpuint (g_variant_n_children (asv), ==, 0);
  g_clear_pointer (&asv, g_variant_unref);
  g_clear_pointer (&creator, g_variant_unref);
  g_clear_pointer (&tuple, g_variant_unref);

  /* Check that the socket is cleaned up when the dbus-daemon is terminated */
  test_kill_pid (f->daemon_pid);
  g_spawn_close_pid (f->daemon_pid);
  f->daemon_pid = 0;

  while (g_stat (f->socket_path, &stat_buf) == 0)
    g_usleep (G_USEC_PER_SEC / 20);

  g_assert_cmpint (errno, ==, ENOENT);

#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * If we are running as root, assert that when one uid (root) creates a
 * container server, another uid (TEST_USER_OTHER) cannot connect to it
 */
static void
test_wrong_uid (Fixture *f,
                gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  GVariant *parameters;

  if (f->skip)
    return;

  parameters = g_variant_new ("(ssa{sv}a{sv})",
                              "com.example.NotFlatpak",
                              "sample-app",
                              NULL, /* no metadata */
                              NULL); /* no named arguments */
  if (!add_container_server (f, g_steal_pointer (&parameters)))
    return;

  g_test_message ("Connecting to %s...", f->socket_dbus_address);
  f->confined_conns[0] = test_try_connect_gdbus_as_user (f->socket_dbus_address,
                                                         TEST_USER_OTHER,
                                                         &f->error);

  /* That might be skipped if we can't become TEST_USER_OTHER */
  if (f->error != NULL &&
      g_error_matches (f->error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    {
      g_test_skip (f->error->message);
      return;
    }

  /* The connection was unceremoniously closed */
  g_assert_error (f->error, G_IO_ERROR, G_IO_ERROR_CLOSED);

#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Test for non-trivial metadata: assert that the metadata a{sv} is
 * carried through correctly, and that the app name is allowed to be empty.
 */
static void
test_metadata (Fixture *f,
               gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  GVariant *asv;
  GVariant *creator;
  GVariant *tuple;
  GVariant *parameters;
  GVariantDict dict;
  const gchar *path_from_query;
  const gchar *name;
  const gchar *type;
  guint32 uid;
  guint u;
  gboolean b;
  const gchar *s;

  if (f->skip)
    return;

  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "Species", "s", "Martes martes");
  g_variant_dict_insert (&dict, "IsCrepuscular", "b", TRUE);
  g_variant_dict_insert (&dict, "NChildren", "u", 2);

  parameters = g_variant_new ("(ss@a{sv}a{sv})",
                              "org.example.Springwatch",
                              /* Verify that empty app names are OK */
                              "",
                              g_variant_dict_end (&dict),
                              NULL); /* no named arguments */
  if (!add_container_server (f, g_steal_pointer (&parameters)))
    return;

  fixture_connect_confined (f, 0);

  g_test_message ("Inspecting connection credentials...");
  tuple = g_dbus_connection_call_sync (f->confined_conns[0], DBUS_SERVICE_DBUS,
                                       DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
                                       "GetConnectionCredentials",
                                       g_variant_new (
                                           "(s)", f->confined_unique_names[0]),
                                       G_VARIANT_TYPE ("(a{sv})"),
                                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                       &f->error);
  g_assert_no_error (f->error);
  g_assert_nonnull (tuple);
  g_assert_cmpstr (g_variant_get_type_string (tuple), ==, "(a{sv})");
  asv = g_variant_get_child_value (tuple, 0);
  g_variant_dict_init (&dict, asv);
  g_assert_true (g_variant_dict_lookup (&dict,
                                        DBUS_INTERFACE_CONTAINERS1 ".Instance",
                                        "&o", &path_from_query));
  g_assert_cmpstr (path_from_query, ==, f->instance_path);
  g_variant_dict_clear (&dict);
  g_clear_pointer (&asv, g_variant_unref);
  g_clear_pointer (&tuple, g_variant_unref);

  g_test_message ("Inspecting connection container info");
  tuple = g_dbus_proxy_call_sync (f->proxy, "GetConnectionInstance",
                                  g_variant_new (
                                      "(s)", f->confined_unique_names[0]),
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, &f->error);
  g_assert_no_error (f->error);
  g_assert_nonnull (tuple);
  g_assert_cmpstr (g_variant_get_type_string (tuple), ==, "(oa{sv}ssa{sv})");
  g_variant_get (tuple, "(&o@a{sv}&s&s@a{sv})",
                 &path_from_query, &creator, &type, &name, &asv);
  g_assert_cmpstr (path_from_query, ==, f->instance_path);
  g_variant_dict_init (&dict, creator);
  g_assert_true (g_variant_dict_lookup (&dict, "UnixUserID", "u", &uid));
  g_assert_cmpuint (uid, ==, _dbus_getuid ());
  g_variant_dict_clear (&dict);
  g_assert_cmpstr (type, ==, "org.example.Springwatch");
  g_assert_cmpstr (name, ==, "");
  g_variant_dict_init (&dict, asv);
  g_assert_true (g_variant_dict_lookup (&dict, "NChildren", "u", &u));
  g_assert_cmpuint (u, ==, 2);
  g_assert_true (g_variant_dict_lookup (&dict, "IsCrepuscular", "b", &b));
  g_assert_cmpint (b, ==, TRUE);
  g_assert_true (g_variant_dict_lookup (&dict, "Species", "&s", &s));
  g_assert_cmpstr (s, ==, "Martes martes");
  g_variant_dict_clear (&dict);
  g_assert_cmpuint (g_variant_n_children (asv), ==, 3);
  g_clear_pointer (&asv, g_variant_unref);
  g_clear_pointer (&creator, g_variant_unref);
  g_clear_pointer (&tuple, g_variant_unref);

  g_test_message ("Inspecting container instance info");
  tuple = g_dbus_proxy_call_sync (f->proxy, "GetInstanceInfo",
                                  g_variant_new ("(o)", f->instance_path),
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, &f->error);
  g_assert_no_error (f->error);
  g_assert_nonnull (tuple);
  g_assert_cmpstr (g_variant_get_type_string (tuple), ==, "(a{sv}ssa{sv})");
  g_variant_get (tuple, "(@a{sv}&s&s@a{sv})", &creator, &type, &name, &asv);
  g_variant_dict_init (&dict, creator);
  g_assert_true (g_variant_dict_lookup (&dict, "UnixUserID", "u", &uid));
  g_assert_cmpuint (uid, ==, _dbus_getuid ());
  g_variant_dict_clear (&dict);
  g_assert_cmpstr (type, ==, "org.example.Springwatch");
  g_assert_cmpstr (name, ==, "");
  g_variant_dict_init (&dict, asv);
  g_assert_true (g_variant_dict_lookup (&dict, "NChildren", "u", &u));
  g_assert_cmpuint (u, ==, 2);
  g_assert_true (g_variant_dict_lookup (&dict, "IsCrepuscular", "b", &b));
  g_assert_cmpint (b, ==, TRUE);
  g_assert_true (g_variant_dict_lookup (&dict, "Species", "&s", &s));
  g_assert_cmpstr (s, ==, "Martes martes");
  g_variant_dict_clear (&dict);
  g_assert_cmpuint (g_variant_n_children (asv), ==, 3);
  g_clear_pointer (&asv, g_variant_unref);
  g_clear_pointer (&creator, g_variant_unref);
  g_clear_pointer (&tuple, g_variant_unref);

#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * With config->stop_server == STOP_SERVER_WITH_MANAGER:
 * Assert that without special parameters, when the container manager
 * disappears from the bus, so does the confined server.
 *
 * With config->stop_server == STOP_SERVER_EXPLICITLY or
 * config->stop_server == STOP_SERVER_DISCONNECT_FIRST:
 * Test StopListening(), which just closes the listening socket.
 *
 * With config->stop_server == STOP_SERVER_FORCE:
 * Test StopInstance(), which closes the listening socket and
 * disconnects all its clients.
 */
static void
test_stop_server (Fixture *f,
                  gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  const Config *config = context;
  GDBusConnection *attacker;
  GDBusConnection *second_confined_conn;
  GDBusProxy *attacker_proxy;
  GSocket *client_socket;
  GSocketAddress *socket_address;
  GVariant *tuple;
  GVariant *parameters;
  gchar *error_name;
  const gchar *name_owner;
  gboolean gone = FALSE;
  guint name_watch;
  guint i;

  g_assert_nonnull (config);

  if (f->skip)
    return;

  f->observer_proxy = g_dbus_proxy_new_sync (f->observer_conn,
                                             G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                             NULL, DBUS_SERVICE_DBUS,
                                             DBUS_PATH_DBUS,
                                             DBUS_INTERFACE_CONTAINERS1, NULL,
                                             &f->error);
  g_assert_no_error (f->error);

  parameters = g_variant_new ("(ssa{sv}a{sv})",
                              "com.example.NotFlatpak",
                              "sample-app",
                              NULL, /* no metadata */
                              NULL); /* no named arguments */
  if (!add_container_server (f, g_steal_pointer (&parameters)))
    return;

  socket_address = g_unix_socket_address_new (f->socket_path);

  if (config->stop_server != STOP_SERVER_NEVER_CONNECTED)
    {
      fixture_connect_confined (f, 0);

      if (config->stop_server == STOP_SERVER_DISCONNECT_FIRST)
        {
          g_test_message ("Disconnecting confined connection...");
          gone = FALSE;
          name_watch = g_bus_watch_name_on_connection (f->observer_conn,
                                                       f->confined_unique_names[0],
                                                       G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                       NULL,
                                                       name_gone_set_boolean_cb,
                                                       &gone, NULL);
          fixture_disconnect_confined (f, 0);

          g_test_message ("Waiting for confined app bus name to disappear...");

          while (!gone)
            g_main_context_iteration (NULL, TRUE);

          g_bus_unwatch_name (name_watch);
        }
    }

  /* If we are able to switch uid (i.e. we are root), check that a local
   * attacker with a different uid cannot close our container instances. */
  attacker = test_try_connect_gdbus_as_user (f->bus_address, TEST_USER_OTHER,
                                             &f->error);

  if (attacker != NULL)
    {
      attacker_proxy = g_dbus_proxy_new_sync (attacker,
                                              G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                              NULL, DBUS_SERVICE_DBUS,
                                              DBUS_PATH_DBUS,
                                              DBUS_INTERFACE_CONTAINERS1, NULL,
                                              &f->error);
      g_assert_no_error (f->error);

      tuple = g_dbus_proxy_call_sync (attacker_proxy, "StopListening",
                                      g_variant_new ("(o)", f->instance_path),
                                      G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                      &f->error);
      g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED);
      g_assert_null (tuple);
      g_clear_error (&f->error);

      tuple = g_dbus_proxy_call_sync (attacker_proxy, "StopInstance",
                                      g_variant_new ("(o)", f->instance_path),
                                      G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                      &f->error);
      g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED);
      g_assert_null (tuple);
      g_clear_error (&f->error);

      g_clear_object (&attacker_proxy);
      g_dbus_connection_close_sync (attacker, NULL, &f->error);
      g_assert_no_error (f->error);
      g_clear_object (&attacker);
    }
  else
    {
      /* If we aren't running as root, it's OK to not be able to connect again
       * as some other user (usually 'nobody'). We don't g_test_skip() here
       * because this is just extra coverage */
      g_assert_error (f->error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
      g_clear_error (&f->error);
    }

  g_assert_false (g_hash_table_contains (f->containers_removed,
                                         f->instance_path));

  switch (config->stop_server)
    {
      case STOP_SERVER_WITH_MANAGER:
        /* Close the unconfined connection (the container manager) and wait
         * for it to go away */
        g_test_message ("Closing container manager...");
        name_watch = g_bus_watch_name_on_connection (f->confined_conns[0],
                                                     f->unconfined_unique_name,
                                                     G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                     NULL,
                                                     name_gone_set_boolean_cb,
                                                     &gone, NULL);
        fixture_disconnect_unconfined (f);

        g_test_message ("Waiting for container manager bus name to disappear...");

        while (!gone)
          g_main_context_iteration (NULL, TRUE);

        g_bus_unwatch_name (name_watch);
        break;

      case STOP_SERVER_EXPLICITLY:
        g_test_message ("Stopping server (but not confined connection)...");
        tuple = g_dbus_proxy_call_sync (f->proxy, "StopListening",
                                        g_variant_new ("(o)", f->instance_path),
                                        G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                        &f->error);
        g_assert_no_error (f->error);
        g_variant_unref (tuple);

        /* The container instance remains open, because the connection has
         * not gone away yet. Do another method call: if we were going to
         * get the signal, it would arrive before the reply to this second
         * method call. Any method will do here, even one that doesn't
         * exist. */
        g_test_message ("Checking we do not get InstanceRemoved...");
        tuple = g_dbus_proxy_call_sync (f->proxy, "NoSuchMethod", NULL,
                                        G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                        &f->error);
        g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD);
        g_assert_null (tuple);
        g_clear_error (&f->error);
        break;

      case STOP_SERVER_DISCONNECT_FIRST:
      case STOP_SERVER_NEVER_CONNECTED:
        g_test_message ("Stopping server (with no confined connections)...");
        tuple = g_dbus_proxy_call_sync (f->proxy, "StopListening",
                                        g_variant_new ("(o)", f->instance_path),
                                        G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                        &f->error);
        g_assert_no_error (f->error);
        g_variant_unref (tuple);

        g_test_message ("Waiting for InstanceRemoved...");
        while (!g_hash_table_contains (f->containers_removed, f->instance_path))
          g_main_context_iteration (NULL, TRUE);

        break;

      case STOP_SERVER_FORCE:
        g_test_message ("Stopping server and all confined connections...");
        tuple = g_dbus_proxy_call_sync (f->proxy, "StopInstance",
                                        g_variant_new ("(o)", f->instance_path),
                                        G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                        &f->error);
        g_assert_no_error (f->error);
        g_variant_unref (tuple);

        g_test_message ("Waiting for InstanceRemoved...");
        while (!g_hash_table_contains (f->containers_removed, f->instance_path))
          g_main_context_iteration (NULL, TRUE);

        break;

      default:
        g_assert_not_reached ();
    }

  /* Now if we try to connect to the server again, it will fail (eventually -
   * closing the socket is not synchronous with respect to the name owner
   * change, so try a few times) */
  for (i = 0; i < 50; i++)
    {
      g_test_message ("Trying to connect to %s again...", f->socket_path);
      client_socket = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                                    G_SOCKET_PROTOCOL_DEFAULT, &f->error);
      g_assert_no_error (f->error);

      if (!g_socket_connect (client_socket, socket_address, NULL, &f->error))
        {
          g_assert_cmpstr (g_quark_to_string (f->error->domain), ==,
                           g_quark_to_string (G_IO_ERROR));

          if (f->error->code != G_IO_ERROR_CONNECTION_REFUSED &&
              f->error->code != G_IO_ERROR_NOT_FOUND)
            g_error ("Unexpected error code %d", f->error->code);

          g_clear_error (&f->error);
          g_clear_object (&client_socket);
          break;
        }

      g_clear_object (&client_socket);
      g_usleep (G_USEC_PER_SEC / 10);
    }

  /* The same thing happens for a D-Bus connection */
  g_test_message ("Trying to connect to %s again...", f->socket_dbus_address);
  second_confined_conn = g_dbus_connection_new_for_address_sync (
      f->socket_dbus_address,
      (G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION |
       G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT),
      NULL, NULL, &f->error);
  g_assert_cmpstr (g_quark_to_string (f->error->domain), ==,
                   g_quark_to_string (G_IO_ERROR));

  if (f->error->code != G_IO_ERROR_CONNECTION_REFUSED &&
      f->error->code != G_IO_ERROR_NOT_FOUND)
    g_error ("Unexpected error code %d", f->error->code);

  g_clear_error (&f->error);
  g_assert_null (second_confined_conn);

  /* The socket has been deleted */
  g_assert_false (g_file_test (f->socket_path, G_FILE_TEST_EXISTS));

  switch (config->stop_server)
    {
      case STOP_SERVER_FORCE:
        g_test_message ("Checking that the confined app gets disconnected...");

        while (!g_dbus_connection_is_closed (f->confined_conns[0]))
          g_main_context_iteration (NULL, TRUE);
        break;

      case STOP_SERVER_DISCONNECT_FIRST:
      case STOP_SERVER_NEVER_CONNECTED:
        /* Nothing to be done here, no confined app is connected */
        break;

      case STOP_SERVER_EXPLICITLY:
      case STOP_SERVER_WITH_MANAGER:
        g_test_message ("Checking that the confined app still works...");
        tuple = g_dbus_connection_call_sync (f->confined_conns[0],
                                             DBUS_SERVICE_DBUS,
                                             DBUS_PATH_DBUS,
                                             DBUS_INTERFACE_DBUS,
                                             "GetNameOwner",
                                             g_variant_new ("(s)",
                                                            DBUS_SERVICE_DBUS),
                                             G_VARIANT_TYPE ("(s)"),
                                             G_DBUS_CALL_FLAGS_NONE, -1,
                                             NULL, &f->error);
        g_assert_no_error (f->error);
        g_assert_nonnull (tuple);
        g_assert_cmpstr (g_variant_get_type_string (tuple), ==, "(s)");
        g_variant_get (tuple, "(&s)", &name_owner);
        g_assert_cmpstr (name_owner, ==, DBUS_SERVICE_DBUS);
        g_clear_pointer (&tuple, g_variant_unref);

        /* The container instance will not disappear from the bus
         * until the confined connection goes away */
        tuple = g_dbus_proxy_call_sync (f->observer_proxy, "GetInstanceInfo",
                                        g_variant_new ("(o)", f->instance_path),
                                        G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                        &f->error);
        g_assert_no_error (f->error);
        g_assert_nonnull (tuple);
        g_clear_pointer (&tuple, g_variant_unref);

        /* Now disconnect the last confined connection, which will make the
         * container instance go away */
        g_test_message ("Closing confined connection...");
        fixture_disconnect_confined (f, 0);
        break;

      default:
        g_assert_not_reached ();
    }

  /* Whatever happened above, by now it has gone away */

  g_test_message ("Waiting for InstanceRemoved...");
  while (!g_hash_table_contains (f->containers_removed, f->instance_path))
    g_main_context_iteration (NULL, TRUE);

  tuple = g_dbus_proxy_call_sync (f->observer_proxy, "GetInstanceInfo",
                                  g_variant_new ("(o)", f->instance_path),
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                  &f->error);
  g_assert_nonnull (f->error);
  error_name = g_dbus_error_get_remote_error (f->error);
  g_assert_cmpstr (error_name, ==, DBUS_ERROR_NOT_CONTAINER);
  g_free (error_name);
  g_assert_null (tuple);
  g_clear_error (&f->error);

#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Assert that we cannot get the container metadata for a path that
 * isn't a container instance, or a bus name that isn't in a container
 * or doesn't exist at all.
 */
static void
test_invalid_metadata_getters (Fixture *f,
                               gconstpointer context)
{
  const gchar *unique_name;
  GVariant *tuple;
  gchar *error_name;

  f->proxy = g_dbus_proxy_new_sync (f->unconfined_conn,
                                    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                    NULL, DBUS_SERVICE_DBUS,
                                    DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1,
                                    NULL, &f->error);
  g_assert_no_error (f->error);

  g_test_message ("Inspecting unconfined connection");
  unique_name = g_dbus_connection_get_unique_name (f->unconfined_conn);
  tuple = g_dbus_proxy_call_sync (f->proxy, "GetConnectionInstance",
                                  g_variant_new ("(s)", unique_name),
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, &f->error);
  g_assert_nonnull (f->error);
  g_assert_null (tuple);
  error_name = g_dbus_error_get_remote_error (f->error);
#ifdef DBUS_ENABLE_CONTAINERS
  g_assert_cmpstr (error_name, ==, DBUS_ERROR_NOT_CONTAINER);
#else
  /* TODO: We can use g_assert_error for this when we depend on GLib 2.42 */
  g_assert_cmpstr (error_name, ==, DBUS_ERROR_UNKNOWN_INTERFACE);
#endif
  g_free (error_name);
  g_clear_error (&f->error);

  g_test_message ("Inspecting a non-connection");
  unique_name = g_dbus_connection_get_unique_name (f->unconfined_conn);
  tuple = g_dbus_proxy_call_sync (f->proxy, "GetConnectionInstance",
                                  g_variant_new ("(s)", "com.example.Nope"),
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, &f->error);
  g_assert_nonnull (f->error);
  g_assert_null (tuple);
#ifdef DBUS_ENABLE_CONTAINERS
  g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER);
#else
  /* TODO: We can use g_assert_error for this when we depend on GLib 2.42 */
  error_name = g_dbus_error_get_remote_error (f->error);
  g_assert_cmpstr (error_name, ==, DBUS_ERROR_UNKNOWN_INTERFACE);
  g_free (error_name);
#endif
  g_clear_error (&f->error);


  g_test_message ("Inspecting container instance info");
  tuple = g_dbus_proxy_call_sync (f->proxy, "GetInstanceInfo",
                                  g_variant_new ("(o)", "/nope"),
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, &f->error);
  g_assert_nonnull (f->error);
  g_assert_null (tuple);
  error_name = g_dbus_error_get_remote_error (f->error);
#ifdef DBUS_ENABLE_CONTAINERS
  g_assert_cmpstr (error_name, ==, DBUS_ERROR_NOT_CONTAINER);
#else
  /* TODO: We can use g_assert_error for this when we depend on GLib 2.42 */
  g_assert_cmpstr (error_name, ==, DBUS_ERROR_UNKNOWN_INTERFACE);
#endif
  g_free (error_name);
  g_clear_error (&f->error);
}

/*
 * Assert that named arguments are validated: passing an unsupported
 * named argument causes an error.
 */
static void
test_unsupported_parameter (Fixture *f,
                            gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  GVariant *tuple;
  GVariant *parameters;
  GVariantDict named_argument_builder;

  if (f->skip)
    return;

  f->proxy = g_dbus_proxy_new_sync (f->unconfined_conn,
                                    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                    NULL, DBUS_SERVICE_DBUS,
                                    DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1,
                                    NULL, &f->error);
  g_assert_no_error (f->error);

  g_variant_dict_init (&named_argument_builder, NULL);
  g_variant_dict_insert (&named_argument_builder,
                         "ThisArgumentIsntImplemented",
                         "b", FALSE);

  parameters = g_variant_new ("(ssa{sv}@a{sv})",
                              "com.example.NotFlatpak",
                              "sample-app",
                              NULL, /* no metadata */
                              g_variant_dict_end (&named_argument_builder));
  tuple = g_dbus_proxy_call_sync (f->proxy, "AddServer",
                                  g_steal_pointer (&parameters),
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, &f->error);

  g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
  g_assert_null (tuple);
  g_clear_error (&f->error);
#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Assert that container types are validated: a container type (container
 * technology) that is not a syntactically valid D-Bus interface name
 * causes an error.
 */
static void
test_invalid_type_name (Fixture *f,
                        gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  GVariant *tuple;
  GVariant *parameters;

  if (f->skip)
    return;

  f->proxy = g_dbus_proxy_new_sync (f->unconfined_conn,
                                    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                    NULL, DBUS_SERVICE_DBUS,
                                    DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1,
                                    NULL, &f->error);
  g_assert_no_error (f->error);

  parameters = g_variant_new ("(ssa{sv}a{sv})",
                              "this is not a valid container type name",
                              "sample-app",
                              NULL, /* no metadata */
                              NULL); /* no named arguments */
  tuple = g_dbus_proxy_call_sync (f->proxy, "AddServer",
                                  g_steal_pointer (&parameters),
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, &f->error);

  g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
  g_assert_null (tuple);
  g_clear_error (&f->error);
#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Assert that a request to create a container server cannot come from a
 * connection to an existing container server.
 * (You cannot put containers in your container so you can sandbox while
 * you sandbox.)
 */
static void
test_invalid_nesting (Fixture *f,
                      gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  GDBusProxy *nested_proxy;
  GVariant *tuple;
  GVariant *parameters;

  if (f->skip)
    return;

  parameters = g_variant_new ("(ssa{sv}a{sv})",
                              "com.example.NotFlatpak",
                              "sample-app",
                              NULL, /* no metadata */
                              NULL); /* no named arguments */
  if (!add_container_server (f, g_steal_pointer (&parameters)))
    return;

  fixture_connect_confined (f, 0);

  g_test_message ("Checking that confined app cannot nest containers...");
  nested_proxy = g_dbus_proxy_new_sync (f->confined_conns[0],
                                        G_DBUS_PROXY_FLAGS_NONE, NULL,
                                        DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
                                        DBUS_INTERFACE_CONTAINERS1, NULL,
                                        &f->error);
  g_assert_no_error (f->error);

  parameters = g_variant_new ("(ssa{sv}a{sv})",
                              "com.example.NotFlatpak",
                              "inner-app",
                              NULL, /* no metadata */
                              NULL); /* no named arguments */
  tuple = g_dbus_proxy_call_sync (nested_proxy, "AddServer",
                                  g_steal_pointer (&parameters),
                                  G_DBUS_CALL_FLAGS_NONE,
                                  -1, NULL, &f->error);

  g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED);
  g_assert_null (tuple);
  g_clear_error (&f->error);

  g_clear_object (&nested_proxy);

#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Assert that we can have up to 3 containers, but no more than that,
 * either because max-containers.conf imposes max_containers=3
 * or because limit-containers.conf imposes max_containers_per_user=3
 * (and we only have one uid).
 */
static void
test_max_containers (Fixture *f,
                     gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  GVariant *parameters;
  GVariant *tuple;
  /* Length must match max_containers in max-containers.conf, and also
   * max_containers_per_user in limit-containers.conf */
  gchar *placeholders[3] = { NULL };
  guint i;

  if (f->skip)
    return;

  f->proxy = g_dbus_proxy_new_sync (f->unconfined_conn,
                                    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                    NULL, DBUS_SERVICE_DBUS,
                                    DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1,
                                    NULL, &f->error);
  g_assert_no_error (f->error);

  parameters = g_variant_new ("(ssa{sv}a{sv})",
                              "com.example.NotFlatpak",
                              "sample-app",
                              NULL, /* no metadata */
                              NULL); /* no named arguments */
  /* We will reuse this variant several times, so don't use floating refs */
  g_variant_ref_sink (parameters);

  /* We can go up to the limit without exceeding it */
  for (i = 0; i < G_N_ELEMENTS (placeholders); i++)
    {
      tuple = g_dbus_proxy_call_sync (f->proxy, "AddServer",
                                      parameters, G_DBUS_CALL_FLAGS_NONE, -1,
                                      NULL, &f->error);
      g_assert_no_error (f->error);
      g_assert_nonnull (tuple);
      g_variant_get (tuple, "(o^ays)", &placeholders[i], NULL, NULL);
      g_variant_unref (tuple);
      g_test_message ("Placeholder server at %s", placeholders[i]);
    }

  /* We cannot exceed the limit */
  tuple = g_dbus_proxy_call_sync (f->proxy, "AddServer",
                                  parameters, G_DBUS_CALL_FLAGS_NONE, -1,
                                  NULL, &f->error);
  g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_LIMITS_EXCEEDED);
  g_clear_error (&f->error);
  g_assert_null (tuple);

  /* Stop one of the placeholders */
  tuple = g_dbus_proxy_call_sync (f->proxy, "StopListening",
                                  g_variant_new ("(o)", placeholders[1]),
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                  &f->error);
  g_assert_no_error (f->error);
  g_assert_nonnull (tuple);
  g_variant_unref (tuple);

  /* We can have another container server now that we are back below the
   * limit */
  tuple = g_dbus_proxy_call_sync (f->proxy, "AddServer",
                                  parameters, G_DBUS_CALL_FLAGS_NONE, -1,
                                  NULL, &f->error);
  g_assert_no_error (f->error);
  g_assert_nonnull (tuple);
  g_variant_unref (tuple);

  g_variant_unref (parameters);

  for (i = 0; i < G_N_ELEMENTS (placeholders); i++)
    g_free (placeholders[i]);

#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

#ifdef HAVE_CONTAINERS_TEST
static void
assert_connection_closed (GError *error)
{
  /* "before 2.44 some "connection closed" errors returned
   * G_IO_ERROR_BROKEN_PIPE, but others returned G_IO_ERROR_FAILED"
   * —GIO documentation */
  if (error->code == G_IO_ERROR_BROKEN_PIPE)
    {
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
    }
  else
    {
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
      g_test_message ("Old GLib: %s", error->message);
      /* This is wrong and bad, but it's the only way to detect this, and
       * the older GLib versions that raised FAILED are no longer a moving
       * target */
      g_assert_true (strstr (error->message, g_strerror (ECONNRESET)) != NULL);
    }
}
#endif

/*
 * Test that if we have multiple app-containers,
 * max_connections_per_container applies to each one individually.
 */
static void
test_max_connections_per_container (Fixture *f,
                                    gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  /* Length is arbitrary */
  gchar *socket_paths[2] = { NULL };
  gchar *dbus_addresses[G_N_ELEMENTS (socket_paths)] = { NULL };
  GSocketAddress *socket_addresses[G_N_ELEMENTS (socket_paths)] = { NULL };
  /* Length must be length of socket_paths * max_connections_per_container in
   * limit-containers.conf */
  GSocket *placeholders[G_N_ELEMENTS (socket_paths) * 3] = { NULL };
  GVariant *parameters;
  GVariant *tuple;
  guint i;

  if (f->skip)
    return;

  f->proxy = g_dbus_proxy_new_sync (f->unconfined_conn,
                                    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                    NULL, DBUS_SERVICE_DBUS,
                                    DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1,
                                    NULL, &f->error);
  g_assert_no_error (f->error);

  parameters = g_variant_new ("(ssa{sv}a{sv})",
                              "com.example.NotFlatpak",
                              "sample-app",
                              NULL, /* no metadata */
                              NULL); /* no named arguments */
  /* We will reuse this variant several times, so don't use floating refs */
  g_variant_ref_sink (parameters);

  for (i = 0; i < G_N_ELEMENTS (socket_paths); i++)
    {
      tuple = g_dbus_proxy_call_sync (f->proxy, "AddServer",
                                      parameters, G_DBUS_CALL_FLAGS_NONE, -1,
                                      NULL, &f->error);
      g_assert_no_error (f->error);
      g_assert_nonnull (tuple);
      g_variant_get (tuple, "(o^ays)", NULL, &socket_paths[i],
                     &dbus_addresses[i]);
      g_variant_unref (tuple);
      socket_addresses[i] = g_unix_socket_address_new (socket_paths[i]);
      g_test_message ("Server #%u at %s", i, socket_paths[i]);
    }

  for (i = 0; i < G_N_ELEMENTS (placeholders); i++)
    {
      /* We enforce the resource limit for any connection to the socket,
       * not just D-Bus connections that have done the handshake */
      placeholders[i] = g_socket_new (G_SOCKET_FAMILY_UNIX,
                                      G_SOCKET_TYPE_STREAM,
                                      G_SOCKET_PROTOCOL_DEFAULT, &f->error);
      g_assert_no_error (f->error);

      g_socket_connect (placeholders[i],
                        socket_addresses[i % G_N_ELEMENTS (socket_paths)],
                        NULL, &f->error);
      g_assert_no_error (f->error);
      g_test_message ("Placeholder connection #%u to %s", i,
                      socket_paths[i % G_N_ELEMENTS (socket_paths)]);
    }

  /* An extra connection to either of the sockets fails: they are both at
   * capacity now */
  for (i = 0; i < G_N_ELEMENTS (socket_paths); i++)
    {
      f->confined_conns[0] = g_dbus_connection_new_for_address_sync (
          dbus_addresses[i],
          (G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION |
           G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT),
          NULL, NULL, &f->error);
      assert_connection_closed (f->error);

      g_clear_error (&f->error);
      g_assert_null (f->confined_conns[0]);
    }

  /* Free up one slot (this happens to be connected to socket_paths[0]) */
  g_socket_close (placeholders[2], &f->error);
  g_assert_no_error (f->error);

  /* Now we can connect, but only once. Use a retry loop since the dbus-daemon
   * won't necessarily notice our socket closing synchronously. */
  while (f->confined_conns[0] == NULL)
    {
      g_test_message ("Trying to use the slot we just freed up...");
      f->confined_conns[0] = g_dbus_connection_new_for_address_sync (
          dbus_addresses[0],
          (G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION |
           G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT),
          NULL, NULL, &f->error);

      if (f->confined_conns[0] == NULL)
        {
          assert_connection_closed (f->error);
          g_clear_error (&f->error);
          g_assert_nonnull (f->confined_conns[0]);
        }
      else
        {
          g_assert_no_error (f->error);
        }
    }

  /* An extra connection to either of the sockets fails: they are both at
   * capacity again */
  for (i = 0; i < G_N_ELEMENTS (socket_paths); i++)
    {
      f->confined_conns[1] = g_dbus_connection_new_for_address_sync (
          dbus_addresses[i],
          (G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION |
           G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT),
          NULL, NULL, &f->error);

      assert_connection_closed (f->error);
      g_clear_error (&f->error);
      g_assert_null (f->confined_conns[1]);
    }

  g_variant_unref (parameters);

  for (i = 0; i < G_N_ELEMENTS (socket_paths); i++)
    {
      g_free (socket_paths[i]);
      g_free (dbus_addresses[i]);
      g_clear_object (&socket_addresses[i]);
    }

#undef LIMIT
#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Assert that the given Allow rules work as intended for the unique
 * name of another connection within the container.
 */
static void
test_allow_see_confined_unique_name (Fixture *f,
                                     gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  GList *iter;
  gboolean saw_connect;
  gboolean saw_disconnect;

  if (f->skip)
    return;

  /* Close confined_conns[1] and assert that
   * confined_conns[0] sees NameOwnerChanged, because connections in
   * the same container always see each other. We can also assert
   * that confined_conns[0] saw NameOwnerChanged when confined_conns[1]
   * connected, because confined_conns[0] was there first. */
  g_test_message ("Checking that confined connection 0 sees "
                  "confined connection 1 gaining/losing unique name");
  fixture_disconnect_confined (f, 1);

  if (g_error_matches (f->error, G_IO_ERROR, G_IO_ERROR_CLOSED))
    g_clear_error (&f->error);
  else
    g_assert_no_error (f->error);

  /* We can't use test_sync_gdbus_connections() here, because one of the
   * connections that's involved has just disconnected, so we have to
   * just wait for it. */
  while (f->confined_1_name_owned != NAME_TRISTATE_NOT_OWNED)
    g_main_context_iteration (NULL, TRUE);

  saw_connect = FALSE;
  saw_disconnect = FALSE;

  for (iter = f->name_owner_changes.head;
       iter != NULL;
       iter = iter->next)
    {
      const NameOwnerChange *noc = iter->data;

      g_test_message ("Past NameOwnerChanged: \"%s\" owner \"%s\" -> \"%s\"",
                      noc->name, noc->old_owner, noc->new_owner);

      if (g_strcmp0 (noc->name, f->confined_unique_names[1]) == 0)
        {
          if (noc->old_owner[0] == '\0')
            {
              g_assert_cmpstr (noc->old_owner, ==, "");
              g_assert_cmpstr (noc->new_owner, ==,
                               f->confined_unique_names[1]);
              g_assert_false (saw_connect);
              g_assert_false (saw_disconnect);
              saw_connect = TRUE;
              g_test_message ("... saw connect");
            }
          else
            {
              g_assert_cmpstr (noc->old_owner, ==,
                               f->confined_unique_names[1]);
              g_assert_cmpstr (noc->new_owner, ==, "");
              g_assert_true (saw_connect);
              g_assert_false (saw_disconnect);
              saw_disconnect = TRUE;
              g_test_message ("... saw disconnect");
            }
        }
    }

  g_assert_true (saw_connect);
  g_assert_true (saw_disconnect);
#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Test what happens when we provide invalid content for the Allow
 * named parameter.
 */
static void
test_invalid_allow_rules (Fixture *f,
                          gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  guint i;
  /* The contents of this array haven't been fully designed yet, but
   * the current assumption is that each rule will be a (usos) struct. */
  static const char * const variants[] =
    {
      "@au []",       /* array of non-structs */
      "@a(uso) []",   /* array of truncated struct */
      "@a(usox) []",  /* array of the wrong struct */
      "@a(usoss) []", /* array of over-long struct */
      "false"         /* not even an array */
    };
  static const AllowRule rules[] =
    {
      /* So far no valid rules have been defined, so anything is
       * invalid; but it's reasonable to assume that the flags being
       * all-ones are not going to be valid any time soon. Similarly,
       * we can confidently say that "nope" is not a valid bus name or
       * a valid interface name. */
      { 0xFFFFFFFFU, "com.example.Valid", "/", "com.example.Valid" },
      { 0, "nope", "/", "com.example.Valid" },
      { 0, "com.example.Valid", "/", "nope" }
    };

  if (f->skip)
    return;

  f->proxy = g_dbus_proxy_new_sync (f->unconfined_conn,
                                    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                    NULL, DBUS_SERVICE_DBUS,
                                    DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1,
                                    NULL, &f->error);
  g_assert_no_error (f->error);

  for (i = 0; i < G_N_ELEMENTS (variants); i++)
    {
      GVariant *tuple;
      GVariant *parameters;
      GVariantDict named_argument_builder;

      g_variant_dict_init (&named_argument_builder, NULL);
      g_variant_dict_insert_value (&named_argument_builder, "Allow",
                                   g_variant_new_parsed (variants[i]));
      /* These are deliberately the same parameters as in test_basic(),
       * except that there is an Allow named parameter, which means
       * the InvalidArgs error must have been caused by the invalid
       * Allow rules. */
      parameters = g_variant_new ("(ssa{sv}@a{sv})",
                                  "com.example.NotFlatpak",
                                  "sample-app",
                                  NULL,
                                  g_variant_dict_end (&named_argument_builder));

      tuple = g_dbus_proxy_call_sync (f->proxy, "AddServer", parameters,
                                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, &f->error);
      g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
      g_assert_null (tuple);
      g_clear_error (&f->error);
    }

  for (i = 0; i < G_N_ELEMENTS (rules); i++)
    {
      GVariant *tuple;
      GVariant *parameters;
      GVariantDict named_argument_builder;

      g_variant_dict_init (&named_argument_builder, NULL);
      g_variant_dict_insert_value (
          &named_argument_builder, "Allow",
          g_variant_new_parsed ("[(%u, %s, %o, %s)]",
                                rules[i].flags,
                                rules[i].bus_name,
                                rules[i].object_path,
                                rules[i].interface_and_maybe_member));
      parameters = g_variant_new ("(ssa{sv}@a{sv})",
                                  "com.example.NotFlatpak",
                                  "sample-app",
                                  NULL,
                                  g_variant_dict_end (&named_argument_builder));

      tuple = g_dbus_proxy_call_sync (f->proxy, "AddServer", parameters,
                                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, &f->error);
      g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
      g_assert_null (tuple);
      g_clear_error (&f->error);
    }

#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Assert that the given Allow rules work as intended for ListNames and
 * ListActivatableNames.
 */
static void
test_allow_list (Fixture *f,
                 gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  const AllowRulesTest *test = context;
  GVariant *reply = NULL;
  guint i;
  gchar **names;

  if (f->skip)
    return;

  /* Use the unconfined (manager) connection to contact a confined
   * connection. This should make the unconfined connection's unique
   * name, but not the observer connection's unique name, visible to
   * all the confined connections. */
  test_sync_gdbus_connections (f->unconfined_conn, f->confined_conns[1]);

  /* When we list owned names, we only see the well-known names we can
   * SEE (by well-known name), plus the unique names we can SEE, plus
   * the bus driver. */
  g_test_message ("Confined connection calling ListNames");
  reply = g_dbus_connection_call_sync (f->confined_conns[0],
                                       DBUS_SERVICE_DBUS,
                                       DBUS_PATH_DBUS,
                                       DBUS_INTERFACE_DBUS,
                                       "ListNames",
                                       NULL, G_VARIANT_TYPE ("(as)"),
                                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                       &f->error);
  g_assert_no_error (f->error);
  g_assert_nonnull (reply);
  g_variant_get (reply, "(^as)", &names, NULL);

  for (i = 0; names[i] != NULL; i++)
    g_test_message ("-> %s", names[i]);

  g_test_message ("-> (end)");

  /* Confined connections can always see the dbus-daemon */
  fixture_assert_name_visibility (f, DBUS_SERVICE_DBUS, TRUE, names);
  /* Confined connections can always see what's in the same container */
  fixture_assert_name_visibility (f, f->confined_unique_names[0], TRUE, names);
  fixture_assert_name_visibility (f, f->confined_unique_names[1], TRUE, names);

  /* The unconfined connection sent messages to us, so that
   * automatically opens up SEE access to its unique name, because
   * otherwise we'd get contradictory answers to our questions and
   * become hopelessly confused.
   */
#if 0
  /* TODO: Not yet implemented */
  fixture_assert_name_visibility (f, f->unconfined_unique_name, TRUE, names);
#endif

  /* We know the observer never sent messages to us in this test,
   * hence its name; so we can see it if and only if we are allowed
   * to see its well-known name. */
  if (allow_rules_test_can_see (test, "com.example.Observer"))
    fixture_assert_name_visibility (f, f->observer_unique_name, TRUE, names);
  else
    fixture_assert_name_visibility (f, f->observer_unique_name, FALSE, names);

  /* When we probe well-known names, we can only see the names we
   * should. Having been sent messages by the unique name that
   * owns that well-known name is not enough. */
  if (allow_rules_test_can_see (test, "com.example.Unconfined"))
    fixture_assert_name_visibility (f, "com.example.Unconfined", TRUE, names);
  else if (allow_rules_test_cannot_see (test, "com.example.Unconfined"))
    fixture_assert_name_visibility (f, "com.example.Unconfined", FALSE, names);
  /* else the test makes no particular statement about that name */

  if (allow_rules_test_can_see (test, "com.example.Observer"))
    fixture_assert_name_visibility (f, "com.example.Observer", TRUE, names);
  else if (allow_rules_test_cannot_see (test, "com.example.Observer"))
    fixture_assert_name_visibility (f, "com.example.Observer", FALSE, names);
  /* else the test makes no particular statement about that name */

  g_strfreev (names);
  g_clear_pointer (&reply, g_variant_unref);

  /* When we list activatable names, we only see the names we can
   * SEE (by well-known name) plus possibly the bus driver. */
  g_test_message ("Confined connection calling ListActivatableNames");
  reply = g_dbus_connection_call_sync (f->confined_conns[0],
                                       DBUS_SERVICE_DBUS,
                                       DBUS_PATH_DBUS,
                                       DBUS_INTERFACE_DBUS,
                                       "ListActivatableNames",
                                       NULL, G_VARIANT_TYPE ("(as)"),
                                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                       &f->error);
  g_assert_no_error (f->error);
  g_assert_nonnull (reply);
  g_variant_get (reply, "(^as)", &names, NULL);

  for (i = 0; names[i] != NULL; i++)
    g_test_message ("-> %s", names[i]);

  g_test_message ("-> (end)");

  /* For each name that is meant to be activatable, if it is one that
   * the test specifies we are allowed to see, we did in fact see it */
  for (i = 0; i < G_N_ELEMENTS (activatable_names); i++)
    {
      const gchar *name = activatable_names[i];

      if (allow_rules_test_can_see (test, name))
        g_assert_true (g_strv_contains ((const gchar * const *) names, name));
      /* else this test makes no particular statement about being
       * allowed to see that name */
    }

  /* For each name we can see as activatable, assert that either it's
   * one we are allowed to see, or the test makes no particular
   * statement about */
  for (i = 0; i < G_N_ELEMENTS (names); i++)
    {
      const gchar *name = names[i];

      if (name == NULL)
        break;

      g_assert_false (allow_rules_test_cannot_see (test, name));
    }

  g_strfreev (names);
  g_clear_pointer (&reply, g_variant_unref);
#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Assert that the given Allow rules work as intended for well-known
 * names owned by the container. If the container can't own any
 * well-known names then this test is impossible.
 */
static void
test_allow_see_confined_well_known_name (Fixture *f,
                                         gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  const AllowRulesTest *test = context;
  GList *iter;
  gboolean saw_acquire;
  gboolean saw_lose;

  if (f->skip)
    return;

  /* We assume the container is allowed to own a name. This test is
   * meaningless otherwise. */
  g_return_if_fail (test->own_name != NULL);

  /* We gave this name to confined_conns[1] earlier, during setup. Drop
   * ownership and assert that we saw NameOwnerChanged for both the
   * acquisition and the loss. */

  g_test_message ("Checking that confined connection 0 saw "
                  "confined connection 1 gaining/losing name %s",
                  test->own_name);
  assert_release_name_succeeds (f->confined_conns[1], test->own_name);

  /* Make sure that if the confined connection was going to get
   * NameOwnerChanged, it would have done so. */
  test_sync_gdbus_connections (f->confined_conns[1], f->confined_conns[0]);

  saw_acquire = FALSE;
  saw_lose = FALSE;

  for (iter = f->name_owner_changes.head;
       iter != NULL;
       iter = iter->next)
    {
      const NameOwnerChange *noc = iter->data;

      g_test_message (
          "Past NameOwnerChanged: \"%s\" owner \"%s\" -> \"%s\"",
          noc->name, noc->old_owner, noc->new_owner);

      if (g_strcmp0 (noc->name, test->own_name) == 0)
        {
          if (noc->old_owner[0] == '\0')
            {
              g_assert_cmpstr (noc->old_owner, ==, "");
              g_assert_cmpstr (noc->new_owner, ==,
                               f->confined_unique_names[1]);
              g_assert_false (saw_acquire);
              g_assert_false (saw_lose);
              saw_acquire = TRUE;
              g_test_message ("... saw acquisition");
            }
          else
            {
              g_assert_cmpstr (noc->old_owner, ==,
                               f->confined_unique_names[1]);
              g_assert_cmpstr (noc->new_owner, ==, "");
              g_assert_true (saw_acquire);
              g_assert_false (saw_lose);
              saw_lose = TRUE;
              g_test_message ("... saw loss");
            }
        }
    }

  g_assert_true (saw_acquire);
  g_assert_true (saw_lose);
#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Assert that the given Allow rules work as intended for names outside
 * the container.
 */
static void
test_allow_see_observer (Fixture *f,
                         gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  const AllowRulesTest *test = context;
  GList *iter;
  gboolean saw_acquire;
  gboolean saw_lose;
  gboolean saw_disconnect;

  if (f->skip)
    return;

  /* When we disconnect some other unconfined connection, we get the
   * NameOwnerChanged for its unique name if and only if we can SEE
   * either its well-known name or unrelated unique names (it never sent
   * a message to us, so we didn't pick up SEE access automatically);
   * and we get the NameOwnerChanged for its well-known name if and
   * only if we can SEE its well-known name.
   *
   * We can't wait for the confined connection to see NameOwnerChanged
   * from non-empty to empty for the observer, because normally it
   * shouldn't have seen that. Instead, we establish causal ordering
   * by waiting for the other unconfined connection to see the observer
   * disappear, then waiting for the other unconfined connection to
   * ping the observer. */

  /* Wait for the unconfined connection to know the observer has its name */
  while (f->observer_unique_name_owned != NAME_TRISTATE_OWNED &&
         f->observer_well_known_name_owned != NAME_TRISTATE_OWNED)
    g_main_context_iteration (NULL, TRUE);

  /* Trigger NameOwnerChanged, if we're allowed to receive it */
  fixture_disconnect_observer (f);

  if (g_error_matches (f->error, G_IO_ERROR, G_IO_ERROR_CLOSED))
    g_clear_error (&f->error);
  else
    g_assert_no_error (f->error);

  /* Wait for the unconfined connection to catch up with the observer
   * connection */
  while (f->observer_unique_name_owned != NAME_TRISTATE_NOT_OWNED &&
         f->observer_well_known_name_owned != NAME_TRISTATE_NOT_OWNED)
    g_main_context_iteration (NULL, TRUE);

  /* Wait for the confined connection to catch up with the unconfined
   * connection */
  test_sync_gdbus_connections (f->unconfined_conn, f->confined_conns[0]);

  /* Assert that we saw the right NameOwnerChanged signals */
  if (allow_rules_test_can_see (test, "com.example.Observer"))
    {
      g_test_message ("Checking that confined connections can see "
                      "NameOwnerChanged for com.example.Observer");
      saw_acquire = FALSE;
      saw_lose = FALSE;
      saw_disconnect = FALSE;

      for (iter = f->name_owner_changes.head;
           iter != NULL;
           iter = iter->next)
        {
          const NameOwnerChange *noc = iter->data;

          if (g_strcmp0 (noc->name, "com.example.Observer") == 0)
            {
              if (noc->old_owner[0] == '\0')
                {
                  g_assert_cmpstr (noc->old_owner, ==, "");
                  g_assert_cmpstr (noc->new_owner, ==, f->observer_unique_name);
                  g_assert_false (saw_acquire);
                  saw_acquire = TRUE;
                }
              else
                {
                  g_assert_cmpstr (noc->old_owner, ==, f->observer_unique_name);
                  g_assert_cmpstr (noc->new_owner, ==, "");
                  g_assert_true (saw_acquire);
                  g_assert_false (saw_lose);
                  saw_lose = TRUE;
                }
            }
          else if (g_strcmp0 (noc->name, f->observer_unique_name) == 0)
            {
              g_assert_cmpstr (noc->old_owner, ==, f->observer_unique_name);
              g_assert_cmpstr (noc->new_owner, ==, "");
              g_assert_true (saw_acquire);
              g_assert_false (saw_disconnect);
              saw_disconnect = TRUE;
            }
        }

      g_assert_true (saw_acquire);
      g_assert_true (saw_lose);
      /* Being able to see the well-known name should imply that we can
       * see the unique name that owned it */
      g_assert_true (saw_disconnect);
    }
  else if (allow_rules_test_cannot_see (test, "com.example.Observer"))
    {
      g_test_message ("Checking that confined connections cannot see "
                      "NameOwnerChanged for com.example.Observer");

      for (iter = f->name_owner_changes.head;
           iter != NULL;
           iter = iter->next)
        {
          const NameOwnerChange *noc = iter->data;

          g_assert_cmpstr (noc->name, !=, f->observer_unique_name);
          g_assert_cmpstr (noc->name, !=, "com.example.Observer");
          g_assert_cmpstr (noc->old_owner, !=, f->observer_unique_name);
          g_assert_cmpstr (noc->new_owner, !=, f->observer_unique_name);
        }
    }
  else
    {
      /* this test makes no particular statement about whether the
       * observer connection is visible to the container */
      g_test_message ("Not checking whether confined connections can see "
                      "NameOwnerChanged for com.example.Observer");
    }

#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Assert that the given Allow rules work as intended for unsolicited
 * replies.
 */
static void
test_allow_no_unsolicited_replies (Fixture *f,
                                   gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  GAsyncResult *result = NULL;
  guint i;
  GDBusMessage *message_with_reply;
  GDBusMessage *message_without_reply;
  GDBusMessage *reply_message;
  guint32 without_reply_serial;
  guint32 with_reply_serial;
  guint use_error_reply;

  if (f->skip)
    return;

  /* Regardless of the ruleset, a container is not allowed to send
   * a reply to a message from outside that was not expecting a reply,
   * or a second reply to a message that was expecting a reply. */

  g_test_message ("Checking that confined connections cannot send "
                  "unsolicited replies");

  message_without_reply = g_dbus_message_new_method_call (
      f->confined_unique_names[0], "/", DBUS_INTERFACE_PEER, "Ping");
  g_dbus_message_set_flags (message_without_reply,
                            G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
  message_with_reply = g_dbus_message_new_method_call (
      f->confined_unique_names[0], "/", DBUS_INTERFACE_PEER, "Ping");

  g_dbus_connection_send_message (f->unconfined_conn,
                                  message_without_reply,
                                  G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                  &without_reply_serial,
                                  &f->error);
  g_assert_no_error (f->error);
  g_assert_cmpuint (without_reply_serial, !=, 0);

  result = NULL;
  g_dbus_connection_send_message_with_reply (f->unconfined_conn,
                                             message_with_reply,
                                             G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                             -1,
                                             &with_reply_serial,
                                             NULL,
                                             test_store_result_cb,
                                             &result);
  g_assert_cmpuint (with_reply_serial, !=, 0);

  while (result == NULL)
    g_main_context_iteration (NULL, TRUE);

  reply_message = g_dbus_connection_send_message_with_reply_finish (
      f->unconfined_conn, result, &f->error);
  g_assert_no_error (f->error);
  g_assert_cmpint (g_dbus_message_get_message_type (reply_message), ==,
                   G_DBUS_MESSAGE_TYPE_METHOD_RETURN);

  /* Sending a completely unexpected reply is forbidden, so this
   * won't arrive at the unconfined connection and trip the check
   * in allow_tests_message_filter(). */
  for (use_error_reply = 0; use_error_reply < 2; use_error_reply++)
    {
      guint32 reply_serials[] =
      {
        /* The serial number of a message that wasn't meant to get
         * a reply */
        without_reply_serial,
        /* The serial number of a message that already had a reply */
        with_reply_serial,
        /* A serial number that hasn't been used yet */
        with_reply_serial + 42
      };

      for (i = 0; i < G_N_ELEMENTS (reply_serials); i++)
        {
          GDBusMessage *unsolicited_reply;
          guint32 reply_serial = reply_serials[i];

          unsolicited_reply = g_dbus_message_new ();
          g_dbus_message_set_destination (unsolicited_reply,
                                          f->unconfined_unique_name);
          g_dbus_message_set_body (unsolicited_reply,
              g_variant_new ("(s)", UNDELIVERABLE_CONTENTS));

          if (use_error_reply == 0)
            {
              g_dbus_message_set_message_type (
                  unsolicited_reply, G_DBUS_MESSAGE_TYPE_METHOD_RETURN);
            }
          else
            {
              g_dbus_message_set_message_type (
                  unsolicited_reply, G_DBUS_MESSAGE_TYPE_ERROR);
              g_dbus_message_set_error_name (unsolicited_reply,
                  "com.example.Pwned");
            }

          g_dbus_message_set_reply_serial (unsolicited_reply,
                                           reply_serial);
          g_dbus_connection_send_message (f->confined_conns[0],
                                          unsolicited_reply,
                                          G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                          NULL, &f->error);
          g_assert_no_error (f->error);

          /* Assert that the confined connection didn't get disconnected,
           * and the unconfined connection that is watching it didn't
           * crash with a g_error() as it would if the unsolicited
           * reply was received */
          test_sync_gdbus_connections (f->unconfined_conn,
                                       f->confined_conns[0]);
        }
    }
#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Assert that the given Allow rules work as intended for method calls,
 * including some special-cased method calls like name ownership.
 */
static void
test_allow_methods (Fixture *f,
                    gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  const AllowRulesTest *test = context;
  GVariant *parameters = NULL;
  GAsyncResult *result = NULL;
  GVariant *reply = NULL;
  guint i;

  if (f->skip)
    return;

  /* This is the data-driven part of the test: try sending a lot of
   * method calls and see what happens. */
  for (i = 0; i < G_N_ELEMENTS (test->method_calls); i++)
    {
      const AllowMethodCall *method_call = &test->method_calls[i];
      const gchar *bus_name = method_call->bus_name;
      const gchar *argument = method_call->argument;
      GDBusConnection *initiator;
      const gchar *initiator_description;

      if (method_call->result == METHOD_INVALID)
        break;

      g_test_message ("%s %s #%d", G_STRFUNC, test->name, i);

      /* do not test g_dbus_is_name() until after we have substituted
       * special strings like REPLACE_WITH_CONFINED_UNIQUE_NAME */
      g_assert (method_call->object_path != NULL);
      g_assert (g_variant_is_object_path (method_call->object_path));
      g_assert (method_call->iface != NULL);
      g_assert (g_dbus_is_interface_name (method_call->iface));
      g_assert (method_call->member != NULL);
      g_assert (g_dbus_is_member_name (method_call->member));

      if (method_call->flags & ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE)
        {
          initiator = f->unconfined_conn;
          initiator_description = "unconfined connection";
        }
      else
        {
          initiator = f->confined_conns[0];
          initiator_description = "confined connection";
        }

      if (g_strcmp0 (bus_name, REPLACE_WITH_CONFINED_UNIQUE_NAME) == 0)
        bus_name = f->confined_unique_names[0];
      else if (g_strcmp0 (bus_name, REPLACE_WITH_CONFINED_1_UNIQUE_NAME) == 0)
        bus_name = f->confined_unique_names[1];
      else if (g_strcmp0 (bus_name, REPLACE_WITH_UNCONFINED_UNIQUE_NAME) == 0)
        bus_name = f->unconfined_unique_name;
      else if (g_strcmp0 (bus_name, REPLACE_WITH_OBSERVER_UNIQUE_NAME) == 0)
        bus_name = f->observer_unique_name;
      else
        g_assert (bus_name == NULL || bus_name[0] != ':');

      g_assert (bus_name == NULL || g_dbus_is_name (bus_name));

      if (g_strcmp0 (argument, REPLACE_WITH_CONFINED_UNIQUE_NAME) == 0)
        argument = f->confined_unique_names[0];
      else if (g_strcmp0 (argument,
                          REPLACE_WITH_CONFINED_1_UNIQUE_NAME) == 0)
        argument = f->confined_unique_names[1];
      else if (g_strcmp0 (argument,
                          REPLACE_WITH_UNCONFINED_UNIQUE_NAME) == 0)
        argument = f->unconfined_unique_name;
      else if (g_strcmp0 (argument,
                          REPLACE_WITH_OBSERVER_UNIQUE_NAME) == 0)
        argument = f->observer_unique_name;

      g_test_message ("%s calling method", initiator_description);

      if (g_strcmp0 (method_call->bus_name, bus_name) != 0)
        g_test_message ("... on %s (%s)", method_call->bus_name, bus_name);
      else
        g_test_message ("... on %s", bus_name);

      g_test_message ("... path %s", method_call->object_path);
      g_test_message ("... %s.%s", method_call->iface, method_call->member);

      if (argument != NULL)
        {
          if (g_strcmp0 (method_call->argument, argument) != 0)
            g_test_message ("... argument \"%s\" (\"%s\")",
                            method_call->argument, argument);
          else
            g_test_message ("... argument \"%s\"", argument);
        }

      if (method_call->flags & ALLOW_MESSAGE_FLAGS_SEND_FD)
        {
          g_assert_null (argument);
          parameters = g_variant_new ("(h)", 0);
          g_test_message ("... argument <fd for /dev/null>");
        }
      else if (g_strcmp0 (method_call->member, "RequestName") == 0)
        {
          g_assert_nonnull (argument);
          parameters = g_variant_new ("(su)",
                                      argument,
                                      DBUS_NAME_FLAG_DO_NOT_QUEUE);
        }
      else if (g_strcmp0 (method_call->member, "StartServiceByName") == 0)
        {
          g_assert_nonnull (argument);
          parameters = g_variant_new ("(su)", argument, 0);
        }
      else if (argument != NULL)
        {
          parameters = g_variant_new ("(s)", argument);
        }

      result = NULL;

      if (method_call->flags & ALLOW_MESSAGE_FLAGS_SEND_FD)
        {
          GUnixFDList *fd_list = new_unix_fd_list ();

          g_dbus_connection_call_with_unix_fd_list (initiator,
              bus_name, method_call->object_path, method_call->iface,
              method_call->member, g_steal_pointer (&parameters), NULL,
              G_DBUS_CALL_FLAGS_NONE, -1, fd_list, NULL,
              test_store_result_cb, &result);
          g_clear_object (&fd_list);

          while (result == NULL)
            g_main_context_iteration (NULL, TRUE);

          reply = g_dbus_connection_call_with_unix_fd_list_finish (
              initiator, NULL, result, &f->error);
        }
      else
        {
          g_dbus_connection_call (initiator, bus_name,
                                  method_call->object_path,
                                  method_call->iface,
                                  method_call->member,
                                  g_steal_pointer (&parameters), NULL,
                                  G_DBUS_CALL_FLAGS_NONE,
                                  -1,
                                  NULL, test_store_result_cb, &result);

          while (result == NULL)
            g_main_context_iteration (NULL, TRUE);

          reply = g_dbus_connection_call_finish (initiator, result, &f->error);
        }

      if (reply != NULL)
        {
          gchar *printable = g_variant_print (reply, TRUE);

          g_test_message ("-> success: %s", printable);
          g_free (printable);
        }
      else
        {
          gchar *printable = g_dbus_error_get_remote_error (f->error);

          g_test_message ("-> error: %s: %s", printable, f->error->message);
          g_free (printable);
        }

      switch (method_call->result)
        {
          case METHOD_SUCCEEDS:
              {
                g_assert_no_error (f->error);
                g_assert_nonnull (reply);
              }
            break;

          case METHOD_ALLOWS_ACCESS:
            if (reply != NULL)
              {
                g_assert_no_error (f->error);
              }
            else
              {
                g_assert_nonnull (f->error);
                g_assert_cmpint (f->error->code, !=,
                                 G_DBUS_ERROR_ACCESS_DENIED);
              }
            break;

          case METHOD_RETURNS_TRUE:
          case METHOD_RETURNS_FALSE:
              {
                gboolean b;

                g_assert_no_error (f->error);
                g_assert_cmpstr (g_variant_get_type_string (reply), ==,
                                 "(b)");
                g_assert_nonnull (reply);
                g_variant_get (reply, "(b)", &b);
                g_assert_cmpint (b, ==,
                                 (method_call->result == METHOD_RETURNS_TRUE));
              }
            break;

          case METHOD_RAISES_NAME_HAS_NO_OWNER:
              {
                g_assert_error (f->error, G_DBUS_ERROR,
                                G_DBUS_ERROR_NAME_HAS_NO_OWNER);
                g_assert_null (reply);
              }
            break;

          case METHOD_RAISES_UNKNOWN_METHOD:
              {
                g_assert_error (f->error, G_DBUS_ERROR,
                                G_DBUS_ERROR_UNKNOWN_METHOD);
                g_assert_null (reply);
              }
            break;

          case METHOD_RAISES_INVALID_ARGS:
              {
                g_assert_error (f->error, G_DBUS_ERROR,
                                G_DBUS_ERROR_INVALID_ARGS);
                g_assert_null (reply);
              }
            break;

          case METHOD_RAISES_ACCESS_DENIED:
              {
                g_assert_error (f->error, G_DBUS_ERROR,
                                G_DBUS_ERROR_ACCESS_DENIED);
                g_assert_null (reply);
              }
            break;

          case METHOD_RAISES_CANNOT_INSPECT:
              {
                g_assert_nonnull (f->error);
                g_assert_null (reply);
                /* We target GLib 2.40, for Ubuntu 14.04 (Travis-CI),
                 * where this is the best we can do. */
                g_assert_cmpint (f->error->code, !=,
                                 G_DBUS_ERROR_ACCESS_DENIED);

#if GLIB_CHECK_VERSION (2, 42, 0)
                /* In GLib >= 2.42 we can be more specific. */
                if (glib_check_version (2, 42, 0))
                  {
                    switch (f->error->code)
                      {
                        case G_DBUS_ERROR_ADT_AUDIT_DATA_UNKNOWN:
                        case G_DBUS_ERROR_SELINUX_SECURITY_CONTEXT_UNKNOWN:
                        case G_DBUS_ERROR_UNIX_PROCESS_ID_UNKNOWN:
                          break;

                        default:
                          g_error ("Unexpected error code %d",
                                   f->error->code);
                      }
                  }
#endif
              }
            break;

          case METHOD_INVALID:
          default:
            g_assert_not_reached ();
        }

      g_clear_error (&f->error);
      g_clear_pointer (&reply, g_variant_unref);
    }
#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Assert that the given Allow rules work as intended for signals.
 */
static void
test_allow_signals (Fixture *f,
                    gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  const AllowRulesTest *test = context;
  guint i;

  if (f->skip)
    return;

  /* This is the data-driven part of the test: try sending a lot of
   * messages and see what happens. */
  for (i = 0; i < G_N_ELEMENTS (test->signals); i++)
    {
      const AllowSignal *signal = &test->signals[i];
      const gchar *bus_name = signal->bus_name;
      GDBusConnection *initiator;
      const gchar *initiator_description;
      GDBusConnection *confined_signal_receiver = NULL;
      GDBusConnection *unconfined_signal_receiver = NULL;
      const gchar *confined_signal_receiver_name = NULL;
      const gchar *unconfined_signal_receiver_name = NULL;
      guint unconfined_subscription = 0;
      guint confined_subscription = 0;
      guint received_confined = 0;
      guint received_unconfined = 0;

      if (signal->result == SIGNAL_INVALID)
        break;

      g_test_message ("%s %s #%d", G_STRFUNC, test->name, i);

      /* do not test g_dbus_is_name() until after we have substituted
       * special strings like REPLACE_WITH_CONFINED_UNIQUE_NAME */
      g_assert (signal->object_path != NULL);
      g_assert (g_variant_is_object_path (signal->object_path));
      g_assert (signal->iface != NULL);
      g_assert (g_dbus_is_interface_name (signal->iface));
      g_assert (signal->member != NULL);
      g_assert (g_dbus_is_member_name (signal->member));
      /* this flag would be meaningless here */
      g_assert (!(signal->flags & ALLOW_MESSAGE_FLAGS_FD_IN_REPLY));

      if (signal->flags & ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE)
        {
          initiator = f->unconfined_conn;
          initiator_description = "unconfined connection";
        }
      else
        {
          initiator = f->confined_conns[0];
          initiator_description = "confined connection";
        }

      if (g_strcmp0 (bus_name, REPLACE_WITH_CONFINED_UNIQUE_NAME) == 0)
        bus_name = f->confined_unique_names[0];
      else if (g_strcmp0 (bus_name, REPLACE_WITH_CONFINED_1_UNIQUE_NAME) == 0)
        bus_name = f->confined_unique_names[1];
      else if (g_strcmp0 (bus_name, REPLACE_WITH_UNCONFINED_UNIQUE_NAME) == 0)
        bus_name = f->unconfined_unique_name;
      else if (g_strcmp0 (bus_name, REPLACE_WITH_OBSERVER_UNIQUE_NAME) == 0)
        bus_name = f->observer_unique_name;
      else
        g_assert (bus_name == NULL || bus_name[0] != ':');

      g_assert (bus_name == NULL || g_dbus_is_name (bus_name));

      g_assert (signal->result == SIGNAL_DELIVERED ||
                signal->result == SIGNAL_NOT_DELIVERED);

      if (signal->flags & ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE)
        {
          /* We only send messages from the unconfined connection if
           * they are going to the first confined connection; there's
           * no point in distinguishing between confined connections,
           * and we aren't testing communication that doesn't involve
           * at least one confined connection. */
          g_assert_true (bus_name == NULL ||
              g_strcmp0 (signal->bus_name,
                  REPLACE_WITH_CONFINED_UNIQUE_NAME) == 0);
          confined_signal_receiver = f->confined_conns[0];
          confined_signal_receiver_name = "confined connection 0";
          unconfined_signal_receiver = f->observer_conn;
          unconfined_signal_receiver_name = "observer";
        }
      else if (bus_name == NULL)
        {
          /* It's a broadcast to unconfined and confined connections.
           * Use the other confined connection to see who
           * receives it. */
          confined_signal_receiver = f->confined_conns[1];
          confined_signal_receiver_name = "confined connection 1";
          unconfined_signal_receiver = f->observer_conn;
          unconfined_signal_receiver_name = "observer";
        }
      else if (g_strcmp0 (signal->bus_name,
                          REPLACE_WITH_CONFINED_1_UNIQUE_NAME) == 0)
        {
          /* It's unicast to the other confined connection */
          confined_signal_receiver = f->confined_conns[1];
          confined_signal_receiver_name = "confined connection 1";
          unconfined_signal_receiver = NULL;
        }
      else
        {
          /* It's unicast to the unconfined connection */
          confined_signal_receiver = NULL;
          g_assert_cmpstr (signal->bus_name, ==,
                           REPLACE_WITH_UNCONFINED_UNIQUE_NAME);
          unconfined_signal_receiver = f->unconfined_conn;
          unconfined_signal_receiver_name = "unconfined connection";
        }

      if (confined_signal_receiver != NULL)
        confined_subscription = g_dbus_connection_signal_subscribe (
            confined_signal_receiver,
            g_dbus_connection_get_unique_name (initiator),
            signal->iface,
            signal->member,
            signal->object_path,
            NULL,
            G_DBUS_SIGNAL_FLAGS_NONE,
            count_signal_cb,
            &received_confined,
            NULL);

      if (unconfined_signal_receiver != NULL)
        unconfined_subscription = g_dbus_connection_signal_subscribe (
            unconfined_signal_receiver,
            g_dbus_connection_get_unique_name (initiator),
            signal->iface,
            signal->member,
            signal->object_path,
            NULL,
            G_DBUS_SIGNAL_FLAGS_NONE,
            count_signal_cb,
            &received_unconfined,
            NULL);

      g_test_message ("%s sending signal", initiator_description);

      if (bus_name == NULL)
        g_test_message ("... broadcast");
      else if (g_strcmp0 (signal->bus_name, bus_name) != 0)
        g_test_message ("... unicast to %s (%s)", signal->bus_name,
                        bus_name);
      else
        g_test_message ("... unicast to %s", bus_name);

      g_test_message ("... path %s", signal->object_path);
      g_test_message ("... %s.%s", signal->iface, signal->member);

      if (signal->flags & ALLOW_MESSAGE_FLAGS_SEND_FD)
        {
          GDBusMessage *s;

          g_test_message ("... with attached file descriptor");
          s = g_dbus_message_new_signal (signal->object_path,
                                         signal->iface,
                                         signal->member);

          if (bus_name != NULL)
            g_dbus_message_set_destination (s, bus_name);

          message_set_body_to_unix_fd (s);
          g_dbus_connection_send_message (initiator, s,
                                          G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                          NULL, &f->error);
          g_object_unref (s);
        }
      else
        {
          g_dbus_connection_emit_signal (initiator,
                                         bus_name,
                                         signal->object_path,
                                         signal->iface,
                                         signal->member,
                                         NULL,
                                         &f->error);
        }

      g_assert_no_error (f->error);

      /* Sync up the connections. Because messages are delivered
       * in-order, if the signal hasn't been received by the time a
       * ping involving the same two connections has completed,
       * then it never will be. */
      if (initiator == f->unconfined_conn)
        {
          if (unconfined_signal_receiver != NULL)
            test_sync_gdbus_connections (initiator,
                                         unconfined_signal_receiver);

          if (confined_signal_receiver != NULL)
            test_sync_gdbus_connections (initiator,
                                         confined_signal_receiver);
        }
      else
        {
          g_assert (initiator == f->confined_conns[0]);

          /* If the initiator was the confined connection, it might
           * not be allowed to contact the unconfined connection,
           * so we do this backwards, using the reply rather than
           * the call to enforce ordering. */
          if (unconfined_signal_receiver != NULL)
            test_sync_gdbus_connections (unconfined_signal_receiver,
                                         initiator);

          if (confined_signal_receiver != NULL)
            test_sync_gdbus_connections (initiator,
                                         confined_signal_receiver);
        }

      if (unconfined_signal_receiver != NULL)
        {
          g_dbus_connection_signal_unsubscribe (unconfined_signal_receiver,
                                                unconfined_subscription);
          g_test_message ("-> %u signal(s) received by %s <%p>",
                          received_unconfined,
                          unconfined_signal_receiver_name,
                          unconfined_signal_receiver);
        }

      if (confined_signal_receiver != NULL)
        {
          g_dbus_connection_signal_unsubscribe (confined_signal_receiver,
                                                confined_subscription);
          g_test_message ("-> %u signal(s) received by %s <%p>",
                          received_confined,
                          confined_signal_receiver_name,
                          confined_signal_receiver);
        }

      if (signal->flags & ALLOW_MESSAGE_FLAGS_INITIATOR_OUTSIDE)
        {
          /* If it was sent from outside, whether it goes to
           * our confined connection is determined by signal->result. */
          g_assert_cmpuint (received_confined, ==,
                            (signal->result == SIGNAL_DELIVERED));
          /* If it was a broadcast, it always goes to unconfined
           * connections (in this case we use the observer)
           * even if it wasn't delivered in the container. */
          if (bus_name == NULL)
            g_assert_cmpuint (received_unconfined, ==, 1);
        }
      else if (bus_name == NULL)
        {
          /* If it's a broadcast, then it always goes to other
           * connections within the container, and signal->result
           * indicates whether it should have left the container. */
          g_assert_cmpuint (received_confined, ==, 1);
          g_assert_cmpuint (received_unconfined, ==,
                            (signal->result == SIGNAL_DELIVERED));
        }
      else
        {
          /* If it's a unicast, then signal->result represents
           * whether it should get to its destination. */
          if (confined_signal_receiver != NULL)
            g_assert_cmpuint (received_confined, ==,
                              (signal->result == SIGNAL_DELIVERED));
          else
            g_assert_cmpuint (received_unconfined, ==,
                              (signal->result == SIGNAL_DELIVERED));
        }
    }
#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

/*
 * Test what happens when we exceed max_container_metadata_bytes.
 * test_metadata() exercises the non-excessive case with the same
 * configuration.
 */
static void
test_max_container_metadata_bytes (Fixture *f,
                                   gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  /* Must be >= max_container_metadata_bytes in limit-containers.conf, so that
   * when the serialization overhead, app-container type and app name are
   * added, it is too much for the limit */
  guchar waste_of_space[4096] = { 0 };
  GVariant *tuple;
  GVariant *parameters;
  GVariantDict dict;

  if (f->skip)
    return;

  f->proxy = g_dbus_proxy_new_sync (f->unconfined_conn,
                                    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                    NULL, DBUS_SERVICE_DBUS,
                                    DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1,
                                    NULL, &f->error);
  g_assert_no_error (f->error);

  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "waste of space", "@ay",
                         g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                    waste_of_space,
                                                    sizeof (waste_of_space),
                                                    1));

  /* Floating reference, call_..._sync takes ownership */
  parameters = g_variant_new ("(ss@a{sv}a{sv})",
                              "com.wasteheadquarters",
                              "Packt Like Sardines in a Crushd Tin Box",
                              g_variant_dict_end (&dict),
                              NULL); /* no named arguments */

  tuple = g_dbus_proxy_call_sync (f->proxy, "AddServer", parameters,
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, &f->error);
  g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_LIMITS_EXCEEDED);
  g_assert_null (tuple);
  g_clear_error (&f->error);

#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("Containers or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

static void
teardown (Fixture *f,
    gconstpointer context G_GNUC_UNUSED)
{
  guint i;

  g_clear_object (&f->proxy);

  fixture_disconnect_observer (f);
  g_clear_pointer (&f->containers_removed, g_hash_table_unref);

  if (f->libdbus_observer != NULL)
    {
      dbus_connection_remove_filter (f->libdbus_observer,
                                     observe_shouting_cb, f);
      dbus_connection_close (f->libdbus_observer);
    }

  dbus_clear_connection (&f->libdbus_observer);

  fixture_disconnect_unconfined (f);

  for (i = 0; i < G_N_ELEMENTS (f->confined_conns); i++)
    fixture_disconnect_confined (f, i);

  if (f->daemon_pid != 0)
    {
      test_kill_pid (f->daemon_pid);
      g_spawn_close_pid (f->daemon_pid);
      f->daemon_pid = 0;
    }

  dbus_clear_message (&f->latest_shout);
  g_free (f->instance_path);
  g_free (f->socket_path);
  g_free (f->socket_dbus_address);
  g_free (f->bus_address);
  g_clear_error (&f->error);
  test_main_context_unref (f->ctx);
  g_queue_foreach (&f->name_owner_changes, (GFunc) name_owner_change_free,
                   NULL);
  g_queue_clear (&f->name_owner_changes);

  g_free (f->unconfined_unique_name);

  for (i = 0; i < G_N_ELEMENTS (f->confined_unique_names); i++)
    g_free (f->confined_unique_names[i]);
}

static const Config stop_server_explicitly =
{
  "valid-config-files/multi-user.conf",
  STOP_SERVER_EXPLICITLY
};
static const Config stop_server_disconnect_first =
{
  "valid-config-files/multi-user.conf",
  STOP_SERVER_DISCONNECT_FIRST
};
static const Config stop_server_never_connected =
{
  "valid-config-files/multi-user.conf",
  STOP_SERVER_NEVER_CONNECTED
};
static const Config stop_server_force =
{
  "valid-config-files/multi-user.conf",
  STOP_SERVER_FORCE
};
static const Config stop_server_with_manager =
{
  "valid-config-files/multi-user.conf",
  STOP_SERVER_WITH_MANAGER
};
static const Config limit_containers =
{
  "valid-config-files/limit-containers.conf",
  0 /* not relevant for this test */
};
static const Config max_containers =
{
  "valid-config-files/max-containers.conf",
  0 /* not relevant for this test */
};

int
main (int argc,
    char **argv)
{
  GError *error = NULL;
  gchar *runtime_dir;
  gchar *runtime_dbus_dir;
  gchar *runtime_containers_dir;
  gchar *runtime_services_dir;
  guint i;
  int ret;

  runtime_dir = g_dir_make_tmp ("dbus-test-containers.XXXXXX", &error);

  if (runtime_dir == NULL)
    {
      g_print ("Bail out! %s\n", error->message);
      g_clear_error (&error);
      return 1;
    }

  g_setenv ("XDG_RUNTIME_DIR", runtime_dir, TRUE);
  runtime_dbus_dir = g_build_filename (runtime_dir, "dbus-1", NULL);
  runtime_containers_dir = g_build_filename (runtime_dir, "dbus-1",
      "containers", NULL);
  runtime_services_dir = g_build_filename (runtime_dir, "dbus-1",
      "services", NULL);

  test_init (&argc, &argv);

  g_test_add ("/containers/get-supported-arguments", Fixture, NULL,
              setup, test_get_supported_arguments, teardown);
  g_test_add ("/containers/basic", Fixture, NULL,
              setup, test_basic, teardown);
  g_test_add ("/containers/wrong-uid", Fixture, NULL,
              setup, test_wrong_uid, teardown);
  g_test_add ("/containers/stop-server/explicitly", Fixture,
              &stop_server_explicitly, setup, test_stop_server, teardown);
  g_test_add ("/containers/stop-server/disconnect-first", Fixture,
              &stop_server_disconnect_first, setup, test_stop_server, teardown);
  g_test_add ("/containers/stop-server/never-connected", Fixture,
              &stop_server_never_connected, setup, test_stop_server, teardown);
  g_test_add ("/containers/stop-server/force", Fixture,
              &stop_server_force, setup, test_stop_server, teardown);
  g_test_add ("/containers/stop-server/with-manager", Fixture,
              &stop_server_with_manager, setup, test_stop_server, teardown);
  g_test_add ("/containers/metadata", Fixture, &limit_containers,
              setup, test_metadata, teardown);
  g_test_add ("/containers/invalid-metadata-getters", Fixture, NULL,
              setup, test_invalid_metadata_getters, teardown);
  g_test_add ("/containers/unsupported-parameter", Fixture, NULL,
              setup, test_unsupported_parameter, teardown);
  g_test_add ("/containers/invalid-type-name", Fixture, NULL,
              setup, test_invalid_type_name, teardown);
  g_test_add ("/containers/invalid-nesting", Fixture, NULL,
              setup, test_invalid_nesting, teardown);
  g_test_add ("/containers/max-containers", Fixture, &max_containers,
              setup, test_max_containers, teardown);
  g_test_add ("/containers/max-containers-per-user", Fixture, &limit_containers,
              setup, test_max_containers, teardown);
  g_test_add ("/containers/max-connections-per-container", Fixture,
              &limit_containers,
              setup, test_max_connections_per_container, teardown);
  g_test_add ("/containers/max-container-metadata-bytes", Fixture,
              &limit_containers,
              setup, test_max_container_metadata_bytes, teardown);
  g_test_add ("/containers/invalid-allow-rules", Fixture, NULL,
              setup, test_invalid_allow_rules, teardown);

  for (i = 0; i < G_N_ELEMENTS (allow_rules_tests); i++)
    {
      const AllowRulesTest *test = &allow_rules_tests[i];
      gchar *path = NULL;

      path = g_strdup_printf ("/containers/allow/%s/see-confined-unique-name",
                              test->name);
      g_test_add (path, Fixture, test,
                  set_up_allow_test, test_allow_see_confined_unique_name,
                  teardown);
      g_free (path);

      path = g_strdup_printf ("/containers/allow/%s/list", test->name);
      g_test_add (path, Fixture, test,
                  set_up_allow_test, test_allow_list, teardown);
      g_free (path);

      if (test->own_name != NULL)
        {
          path = g_strdup_printf (
              "/containers/allow/%s/see-confined-well-known-name",
              test->name);
          g_test_add (path, Fixture, test,
                      set_up_allow_test,
                      test_allow_see_confined_well_known_name,
                      teardown);
          g_free (path);
        }

      path = g_strdup_printf ("/containers/allow/%s/see-observer",
                              test->name);
      g_test_add (path, Fixture, test,
                  set_up_allow_test, test_allow_see_observer, teardown);
      g_free (path);

      path = g_strdup_printf ("/containers/allow/%s/no-unsolicited-replies",
                              test->name);
      g_test_add (path, Fixture, test,
                  set_up_allow_test, test_allow_no_unsolicited_replies,
                  teardown);
      g_free (path);

      path = g_strdup_printf ("/containers/allow/%s/methods", test->name);
      g_test_add (path, Fixture, test,
                  set_up_allow_test, test_allow_methods, teardown);
      g_free (path);

      path = g_strdup_printf ("/containers/allow/%s/signals", test->name);
      g_test_add (path, Fixture, test,
                  set_up_allow_test, test_allow_signals, teardown);
      g_free (path);
    }

  ret = g_test_run ();

  test_rmdir_if_exists (runtime_containers_dir);
  test_rmdir_if_exists (runtime_services_dir);
  test_rmdir_if_exists (runtime_dbus_dir);
  test_rmdir_must_exist (runtime_dir);
  g_free (runtime_containers_dir);
  g_free (runtime_services_dir);
  g_free (runtime_dbus_dir);
  g_free (runtime_dir);
  return ret;
}
