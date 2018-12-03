set(TEST_DIR ../../test)

include_directories(${TEST_DIR})

add_definitions(${DBUS_INTERNAL_CLIENT_DEFINITIONS})

set(DBUS_SESSION_BUS_LISTEN_ADDRESS ${TEST_LISTEN})

add_library(dbus-testutils STATIC
    ${TEST_DIR}/disable-crash-handling.c
    ${TEST_DIR}/disable-crash-handling.h
    ${TEST_DIR}/test-utils.h
    ${TEST_DIR}/test-utils.c
)
target_link_libraries(dbus-testutils ${DBUS_INTERNAL_LIBRARIES})

add_subdirectory( name-test )

set (manual-dir-iter_SOURCES
    ${TEST_DIR}/manual-dir-iter.c
)

set (test-service_SOURCES
    ${TEST_DIR}/test-service.c
)

set (test-names_SOURCES
    ${TEST_DIR}/test-names.c
)

set (break_loader_SOURCES
    ${TEST_DIR}/break-loader.c
)

set (test-shell-service_SOURCES
    ${TEST_DIR}/test-shell-service.c
)

set (test-shell_SOURCES
        ${TEST_DIR}/shell-test.c
)

set (test-spawn_SOURCES
    ${TEST_DIR}/spawn-test.c
)

set (test-exit_SOURCES
    ${TEST_DIR}/test-exit.c
)

# We have to compile a separate copy of disable-crash-handling.c for
# test-segfault rather than using the libdbus-testutils library, because
# otherwise it would fail to link when using the AddressSanitizer.
set (test-segfault_SOURCES
    ${TEST_DIR}/disable-crash-handling.c
    ${TEST_DIR}/disable-crash-handling.h
    ${TEST_DIR}/test-segfault.c
)

set (test-sleep-forever_SOURCES
    ${TEST_DIR}/test-sleep-forever.c
)

set (manual-tcp_SOURCES
    ${TEST_DIR}/manual-tcp.c
)

set (manual-paths_SOURCES
    ${TEST_DIR}/manual-paths.c
)

add_helper_executable(manual-dir-iter ${manual-dir-iter_SOURCES} ${DBUS_INTERNAL_LIBRARIES})
add_helper_executable(test-service ${test-service_SOURCES} dbus-testutils)
add_helper_executable(test-names ${test-names_SOURCES} dbus-testutils)
add_test_executable(test-shell ${test-shell_SOURCES} ${DBUS_INTERNAL_LIBRARIES})
add_test_executable(test-printf ${TEST_DIR}/internals/printf.c dbus-testutils)
add_helper_executable(test-shell-service ${test-shell-service_SOURCES} dbus-testutils)
add_helper_executable(test-spawn ${test-spawn_SOURCES} ${DBUS_INTERNAL_LIBRARIES})
add_helper_executable(test-exit ${test-exit_SOURCES} ${DBUS_INTERNAL_LIBRARIES})
# the second argument of add_helper_executable() is a whitespace-separated
# list of source files and the third and subsequent arguments are libraries
# to link, hence the quoting here
add_helper_executable(test-segfault "${test-segfault_SOURCES}")
add_helper_executable(test-sleep-forever ${test-sleep-forever_SOURCES} ${DBUS_INTERNAL_LIBRARIES})
add_helper_executable(manual-tcp ${manual-tcp_SOURCES} ${DBUS_INTERNAL_LIBRARIES})
add_helper_executable(manual-backtrace ${TEST_DIR}/manual-backtrace.c dbus-1)
if(WIN32)
    add_helper_executable(manual-paths ${manual-paths_SOURCES} ${DBUS_INTERNAL_LIBRARIES})
endif()

if(DBUS_WITH_GLIB)
    message(STATUS "with glib test apps")

    add_library(dbus-testutils-glib STATIC
        ${TEST_DIR}/test-utils-glib.h
        ${TEST_DIR}/test-utils-glib.c
    )
    target_link_libraries(dbus-testutils-glib dbus-testutils ${DBUS_INTERNAL_LIBRARIES})

    add_definitions(
        ${GLIB2_DEFINITIONS}
    )
    include_directories(
        ${GLIB2_INCLUDE_DIR}
    )

    set(TEST_LIBRARIES ${DBUS_INTERNAL_LIBRARIES} dbus-testutils dbus-testutils-glib ${GLIB2_LIBRARIES})

    add_test_executable(test-assertions ${TEST_DIR}/internals/assertions.c ${TEST_LIBRARIES})
    add_test_executable(test-corrupt ${TEST_DIR}/corrupt.c ${TEST_LIBRARIES})
    add_test_executable(test-dbus-daemon ${TEST_DIR}/dbus-daemon.c ${TEST_LIBRARIES})
    add_test_executable(test-dbus-daemon-eavesdrop ${TEST_DIR}/dbus-daemon-eavesdrop.c ${TEST_LIBRARIES})
    add_test_executable(test-desktop-file ${TEST_DIR}/internals/desktop-file.c ${TEST_LIBRARIES})
    add_test_executable(test-fdpass ${TEST_DIR}/fdpass.c ${TEST_LIBRARIES})
    add_test_executable(test-loopback ${TEST_DIR}/loopback.c ${TEST_LIBRARIES})
    add_test_executable(test-marshal ${TEST_DIR}/marshal.c ${TEST_LIBRARIES})
    add_test_executable(test-monitor ${TEST_DIR}/monitor.c ${TEST_LIBRARIES})
    add_test_executable(test-refs ${TEST_DIR}/internals/refs.c ${TEST_LIBRARIES})
    add_test_executable(test-relay ${TEST_DIR}/relay.c ${TEST_LIBRARIES})
    add_test_executable(test-server-oom ${TEST_DIR}/internals/server-oom.c ${TEST_LIBRARIES})
    add_test_executable(test-syntax ${TEST_DIR}/syntax.c ${TEST_LIBRARIES})
    add_test_executable(test-sysdeps ${TEST_DIR}/internals/sysdeps.c ${TEST_LIBRARIES})
    add_test_executable(test-syslog ${TEST_DIR}/internals/syslog.c ${TEST_LIBRARIES})
    add_test_executable(test-uid-permissions ${TEST_DIR}/uid-permissions.c ${TEST_LIBRARIES})
    add_helper_executable(manual-authz ${TEST_DIR}/manual-authz.c ${TEST_LIBRARIES})
    add_helper_executable(manual-test-thread-blocking ${TEST_DIR}/thread-blocking.c ${TEST_LIBRARIES})
endif()

### keep these in creation order, i.e. uppermost dirs first 
set (TESTDIRS
    test/data
    test/data/invalid-messages
    test/data/auth
    test/data/sha-1
    test/data/systemd-activation
    test/data/valid-config-files
    test/data/valid-config-files/basic.d
    test/data/valid-config-files/session.d
    test/data/valid-config-files-system
    test/data/valid-config-files-system/system.d
    test/data/valid-service-files
    test/data/valid-service-files-system
    test/data/invalid-config-files
    test/data/invalid-config-files-system
    test/data/invalid-messages
    test/data/invalid-service-files-system
    test/data/equiv-config-files
    test/data/equiv-config-files/basic
    test/data/equiv-config-files/basic/basic.d
    test/data/equiv-config-files/entities
    test/data/equiv-config-files/entities/basic.d
)
set (CONFIG_VERBOSE 0)

FOREACH(DIR ${TESTDIRS})
    FILE(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/${DIR})
ENDFOREACH(DIR)

### copy tests to builddir so that generated tests and static tests 
### are all in one place.
MESSAGE(STATUS "Copying test files to test directory")
FOREACH(FILE_TYPE *.message-raw *.auth-script *.sha1 *.txt *.conf *.service)
    FOREACH(DIR ${TESTDIRS})
        FILE(GLOB FILES "${CMAKE_SOURCE_DIR}/../${DIR}/${FILE_TYPE}" )
        FOREACH(FILE ${FILES})
            GET_FILENAME_COMPONENT(FILENAME ${FILE} NAME)
            SET (TARGET ${CMAKE_BINARY_DIR}/${DIR}/${FILENAME})
            configure_file(${FILE} ${TARGET} COPYONLY)
            IF (CONFIG_VERBOSE)
                MESSAGE("${FILE}")
            ENDIF (CONFIG_VERBOSE)
        ENDFOREACH(FILE)
    ENDFOREACH(DIR)
ENDFOREACH(FILE_TYPE)

### generate test files
MESSAGE(STATUS "Generating test files from templates into test directory")

FOREACH(FILE_TYPE *.conf.in *.service.in)
  FOREACH(DIR ${TESTDIRS})
    FILE(GLOB FILES "${CMAKE_SOURCE_DIR}/../${DIR}/${FILE_TYPE}" )
    FOREACH(FILE ${FILES})
      GET_FILENAME_COMPONENT(FILENAME ${FILE} NAME)
      STRING(REGEX REPLACE "\\.in$" "" FILENAME ${FILENAME})
      SET (TARGET ${CMAKE_BINARY_DIR}/${DIR}/${FILENAME})
      configure_file(${FILE} ${TARGET} @ONLY IMMEDIATE)
      IF (CONFIG_VERBOSE)
        MESSAGE("${FILE}")
      ENDIF (CONFIG_VERBOSE)
    ENDFOREACH(FILE)
  ENDFOREACH(DIR)
ENDFOREACH(FILE_TYPE)

MESSAGE(STATUS "Copying generated bus config files to test directory")
configure_file("${CMAKE_SOURCE_DIR}/../bus/session.conf.in" ${CMAKE_BINARY_DIR}/test/data/valid-config-files/session.conf @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/../bus/system.conf.in" ${CMAKE_BINARY_DIR}/test/data/valid-config-files-system/system.conf @ONLY)
