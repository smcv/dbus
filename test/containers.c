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

#if defined(DBUS_ENABLE_CONTAINERS) && defined(HAVE_GIO_UNIX)
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

    GDBusConnection *unconfined_conn;
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
  g_test_add ("/containers/unsupported-parameter", Fixture, NULL,
              setup, test_unsupported_parameter, teardown);
  g_test_add ("/containers/invalid-type-name", Fixture, NULL,
              setup, test_invalid_type_name, teardown);

  return g_test_run ();
}
