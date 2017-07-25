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

#include <errno.h>

#include <dbus/dbus.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#if defined(DBUS_ENABLE_CONTAINERS) && defined(HAVE_GIO_UNIX)

#define HAVE_CONTAINERS_TEST

#include <gio/gunixfdlist.h>
#include <gio/gunixsocketaddress.h>

#include "dbus/dbus-sysdeps-unix.h"

#endif

#include "test-utils-glib.h"

typedef struct {
    gboolean skip;
    gchar *bus_address;
    GPid daemon_pid;
    GError *error;

    GDBusProxy *proxy;

    gchar *instance_path;
    gchar *socket_path;
    gchar *socket_dbus_address;
    GDBusConnection *unconfined_conn;
    GDBusConnection *confined_conn;
} Fixture;

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
 */
static void
test_basic (Fixture *f,
            gconstpointer context)
{
#ifdef HAVE_CONTAINERS_TEST
  GVariant *parameters;
  const gchar *manager_unique_name;
  const gchar *name_owner;
  GStatBuf stat_buf;
  GVariant *tuple;

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
  f->confined_conn = g_dbus_connection_new_for_address_sync (
      f->socket_dbus_address,
      (G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION |
       G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT),
      NULL, NULL, &f->error);
  g_assert_no_error (f->error);

  g_test_message ("Making a method call from confined app...");
  tuple = g_dbus_connection_call_sync (f->confined_conn, DBUS_SERVICE_DBUS,
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
  manager_unique_name = g_dbus_connection_get_unique_name (f->unconfined_conn);
  tuple = g_dbus_connection_call_sync (f->confined_conn, manager_unique_name,
                                       "/", DBUS_INTERFACE_PEER,
                                       "Ping",
                                       NULL, G_VARIANT_TYPE_UNIT,
                                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                       &f->error);
  g_assert_no_error (f->error);
  g_assert_nonnull (tuple);
  g_assert_cmpstr (g_variant_get_type_string (tuple), ==, "()");
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

static void
teardown (Fixture *f,
    gconstpointer context G_GNUC_UNUSED)
{
  g_clear_object (&f->proxy);

  if (f->unconfined_conn != NULL)
    {
      GError *error = NULL;

      g_dbus_connection_close_sync (f->unconfined_conn, NULL, &error);

      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CLOSED))
        g_clear_error (&error);
      else
        g_assert_no_error (error);
    }

  g_clear_object (&f->unconfined_conn);

  if (f->confined_conn != NULL)
    {
      GError *error = NULL;

      g_dbus_connection_close_sync (f->confined_conn, NULL, &error);

      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CLOSED))
        g_clear_error (&error);
      else
        g_assert_no_error (error);
    }

  g_clear_object (&f->confined_conn);

  if (f->daemon_pid != 0)
    {
      test_kill_pid (f->daemon_pid);
      g_spawn_close_pid (f->daemon_pid);
      f->daemon_pid = 0;
    }

  g_free (f->instance_path);
  g_free (f->socket_path);
  g_free (f->socket_dbus_address);
  g_free (f->bus_address);
  g_clear_error (&f->error);
}

int
main (int argc,
    char **argv)
{
  GError *error = NULL;
  gchar *runtime_dir;
  gchar *runtime_dbus_dir;
  gchar *runtime_containers_dir;
  gchar *runtime_services_dir;
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
  g_test_add ("/containers/unsupported-parameter", Fixture, NULL,
              setup, test_unsupported_parameter, teardown);
  g_test_add ("/containers/invalid-type-name", Fixture, NULL,
              setup, test_invalid_type_name, teardown);

  ret = g_test_run ();

  test_rmdir_if_exists (runtime_containers_dir);
  test_rmdir_if_exists (runtime_services_dir);
  test_rmdir_if_exists (runtime_dbus_dir);
  test_rmdir_must_exist (runtime_dir);
  return ret;
}
