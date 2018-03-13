#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <stdio.h>
#include <stdlib.h>

#include <dbus/dbus.h>

#include <dbus/dbus-mainloop.h>
#include <dbus/dbus-internals.h>
typedef DBusLoop TestMainContext;

_DBUS_WARN_UNUSED_RESULT
TestMainContext *test_main_context_get            (void);
_DBUS_WARN_UNUSED_RESULT
TestMainContext *test_main_context_try_get        (void);
TestMainContext *test_main_context_ref            (TestMainContext *ctx);
void             test_main_context_unref          (TestMainContext *ctx);
void             test_main_context_iterate        (TestMainContext *ctx,
                                                   dbus_bool_t      may_block);

_DBUS_WARN_UNUSED_RESULT
dbus_bool_t test_connection_try_setup             (TestMainContext *ctx,
                                                   DBusConnection *connection);
void        test_connection_setup                 (TestMainContext *ctx,
                                                   DBusConnection *connection);
void        test_connection_shutdown              (TestMainContext *ctx,
                                                   DBusConnection *connection);

_DBUS_WARN_UNUSED_RESULT
dbus_bool_t test_server_try_setup                 (TestMainContext *ctx,
                                                   DBusServer    *server);
void        test_server_setup                     (TestMainContext *ctx,
                                                   DBusServer    *server);
void        test_server_shutdown                  (TestMainContext *ctx,
                                                   DBusServer    *server);
void        test_pending_call_store_reply         (DBusPendingCall *pc,
                                                   void *data);

#endif
