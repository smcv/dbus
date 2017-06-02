/* Integration tests for restricted sockets for containers
 *
 * Copyright © 2017 Collabora Ltd.
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

#include <dbus/dbus.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#if defined(DBUS_UNIX) && defined(HAVE_UNIX_FD_PASSING) && defined(HAVE_GIO_UNIX)
#define HAVE_CONTAINERS_TEST
#include <gio/gunixfdlist.h>
#include <gio/gunixsocketaddress.h>
#endif

#include "test-utils-glib.h"

typedef struct {
    gboolean skip;
    gchar *bus_address;
    GPid daemon_pid;
    GError *error;

    GDBusProxy *proxy;

    GUnixFDList *fds;
    gchar *temp_dir;
    gchar *socket_path;
    gchar *socket_dbus_address;
    GSocketAddress *socket_address;
    gint32 handle;

    GDBusConnection *unconfined_conn;
} Fixture;

static void
fixture_listen (Fixture *f)
{
  GSocket *socket;
  gchar *path_escaped;

  f->temp_dir = g_dir_make_tmp ("dbus-test.XXXXXX", &f->error);
  g_assert_no_error (f->error);
  f->socket_path = g_build_filename (f->temp_dir, "socket", NULL);
  f->socket_address = g_unix_socket_address_new (f->socket_path);
  socket = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                         G_SOCKET_PROTOCOL_DEFAULT, &f->error);
  g_assert_no_error (f->error);
  g_socket_bind (socket, G_SOCKET_ADDRESS (f->socket_address), FALSE,
                 &f->error);
  g_assert_no_error (f->error);
  g_socket_listen (socket, &f->error);
  g_assert_no_error (f->error);
  path_escaped = g_dbus_address_escape_value (f->socket_path);
  f->socket_dbus_address = g_strdup_printf ("unix:path=%s", path_escaped);
  g_free (path_escaped);

  f->fds = g_unix_fd_list_new ();
  /* This actually dup()s the socket fd */
  f->handle = g_unix_fd_list_append (f->fds, g_socket_get_fd (socket),
                                     &f->error);
  g_assert_no_error (f->error);

  g_socket_close (socket, &f->error);
  g_assert_no_error (f->error);
  g_clear_object (&socket);
}

static void
setup (Fixture *f,
       gconstpointer context)
{
  f->bus_address = test_get_dbus_daemon (NULL, TEST_USER_ME, NULL,
                                         &f->daemon_pid);

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
}

static void
test_get_supported_arguments (Fixture *f,
                              gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  GVariant *v;
  const gchar **args;
  gsize len;
#endif

  if (f->skip)
    return;

  f->proxy = g_dbus_proxy_new_sync (f->unconfined_conn, G_DBUS_PROXY_FLAGS_NONE,
                                    NULL, DBUS_SERVICE_DBUS,
                                    DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1,
                                    NULL, &f->error);

  /* This one is HAVE_UNIX_FD_PASSING rather than HAVE_CONTAINERS_TEST
   * because we can still test whether the interface appears or not, even
   * if we were not able to detect gio-unix-2.0 */
#ifdef HAVE_UNIX_FD_PASSING
  g_assert_no_error (f->error);

  v = g_dbus_proxy_get_cached_property (f->proxy, "SupportedArguments");
  g_assert_cmpstr (g_variant_get_type_string (v), ==, "as");
  args = g_variant_get_strv (v, &len);

  /* No arguments are defined yet */
  g_assert_cmpuint (len, ==, 0);

  g_free (args);
  g_variant_unref (v);
#else /* !HAVE_UNIX_FD_PASSING */
  g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE);
#endif /* !HAVE_UNIX_FD_PASSING */
}

static void
test_basic (Fixture *f,
            gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  GVariant *tuple;
  GVariant *parameters;

  if (f->skip)
    return;

  fixture_listen (f);
  f->proxy = g_dbus_proxy_new_sync (f->unconfined_conn,
                                    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                    NULL, DBUS_SERVICE_DBUS,
                                    DBUS_PATH_DBUS, DBUS_INTERFACE_CONTAINERS1,
                                    NULL, &f->error);
  g_assert_no_error (f->error);

  /* Floating reference, call_..._sync takes ownership */
  parameters = g_variant_new ("(ssa{sv}ha{sv})",
                              "com.example.NotFlatpak",
                              "sample-app",
                              NULL, /* no metadata */
                              f->handle,
                              NULL); /* no named arguments */

  g_test_message ("Calling AddContainerServer...");
  tuple = g_dbus_proxy_call_with_unix_fd_list_sync (f->proxy,
                                                    "AddContainerServer",
                                                    parameters,
                                                    G_DBUS_CALL_FLAGS_NONE,
                                                    -1, f->fds, NULL, NULL,
                                                    &f->error);

  /* It's just a stub implementation at the moment */
  g_assert_error (f->error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED);
  g_assert_null (tuple);
  g_clear_error (&f->error);
#else /* !HAVE_CONTAINERS_TEST */
  g_test_skip ("fd-passing or gio-unix-2.0 not supported");
#endif /* !HAVE_CONTAINERS_TEST */
}

static void
teardown (Fixture *f,
    gconstpointer context G_GNUC_UNUSED)
{
  g_clear_object (&f->fds);
  g_clear_object (&f->proxy);
  g_clear_object (&f->socket_address);
  g_free (f->socket_dbus_address);

  if (f->socket_path != NULL)
    {
      g_unlink (f->socket_path);
      g_free (f->socket_path);
    }

  if (f->temp_dir != NULL)
    {
      g_rmdir (f->temp_dir);
      g_free (f->temp_dir);
    }

  if (f->unconfined_conn != NULL)
    {
      GError *error = NULL;

      g_dbus_connection_close_sync (f->unconfined_conn, NULL, &error);
      g_assert_no_error (error);
    }

  g_clear_object (&f->unconfined_conn);

  if (f->daemon_pid != 0)
    {
      test_kill_pid (f->daemon_pid);
      g_spawn_close_pid (f->daemon_pid);
      f->daemon_pid = 0;
    }

  g_free (f->bus_address);
  g_clear_error (&f->error);
}

int
main (int argc,
    char **argv)
{
  test_init (&argc, &argv);

  g_test_add ("/containers/get-supported-arguments", Fixture, NULL,
              setup, test_get_supported_arguments, teardown);
  g_test_add ("/containers/basic", Fixture, NULL,
              setup, test_basic, teardown);

  return g_test_run ();
}
