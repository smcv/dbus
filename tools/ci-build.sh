#!/bin/bash

# Copyright © 2015-2016 Collabora Ltd.
#
# Permission is hereby granted, free of charge, to any person
# obtaining a copy of this software and associated documentation files
# (the "Software"), to deal in the Software without restriction,
# including without limitation the rights to use, copy, modify, merge,
# publish, distribute, sublicense, and/or sell copies of the Software,
# and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
# BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
# ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

set -euo pipefail
set -x

NULL=

# ci_buildsys:
# Build system under test: autotools or cmake
: "${ci_buildsys:=autotools}"

# ci_docker:
# If non-empty, this is the name of a Docker image. ci-install.sh will
# fetch it with "docker pull" and use it as a base for a new Docker image
# named "ci-image" in which we will do our testing.
#
# If empty, we test on "bare metal".
# Typical values: ubuntu:xenial, debian:jessie-slim
: "${ci_docker:=}"

# ci_host:
# See ci-install.sh
: "${ci_host:=native}"

# ci_parallel:
# A number of parallel jobs, passed to make -j
: "${ci_parallel:=1}"

# ci_sudo:
# If yes, assume we can get root using sudo; if no, only use current user
: "${ci_sudo:=no}"

# ci_test:
# If yes, run tests; if no, just build
: "${ci_test:=yes}"

# ci_test_fatal:
# If yes, test failures break the build; if no, they are reported but ignored
: "${ci_test_fatal:=yes}"

# ci_variant:
# One of debug, reduced, legacy, production
: "${ci_variant:=production}"

if [ -n "$ci_docker" ]; then
    exec docker run \
        --env=ci_buildsys="${ci_buildsys}" \
        --env=ci_docker="" \
        --env=ci_host="${ci_host}" \
        --env=ci_parallel="${ci_parallel}" \
        --env=ci_sudo=yes \
        --env=ci_test="${ci_test}" \
        --env=ci_test_fatal="${ci_test_fatal}" \
        --env=ci_variant="${ci_variant}" \
        --privileged \
        ci-image \
        tools/ci-build.sh
fi

maybe_fail_tests () {
    if [ "$ci_test_fatal" = yes ]; then
        exit 1
    fi
}

NOCONFIGURE=1 ./autogen.sh

srcdir="$(pwd)"
mkdir ci-build-${ci_variant}-${ci_host}
cd ci-build-${ci_variant}-${ci_host}

make="make -j${ci_parallel} V=1 VERBOSE=1"

case "$ci_host" in
    (mingw)
        mirror=http://repo.msys2.org/mingw/i686
        mingw="$(pwd)/mingw32"
        install -d "${mingw}"
        export PKG_CONFIG_LIBDIR="${mingw}/lib/pkgconfig"
        export PKG_CONFIG_PATH=
        export PKG_CONFIG="pkg-config --define-variable=prefix=${mingw}"
        unset CC
        unset CXX
        for pkg in \
            expat-2.1.0-6 \
            gcc-libs-5.2.0-4 \
            gettext-0.19.6-1 \
            glib2-2.46.1-1 \
            libffi-3.2.1-3 \
            zlib-1.2.8-9 \
            ; do
            wget ${mirror}/mingw-w64-i686-${pkg}-any.pkg.tar.xz
            tar -xvf mingw-w64-i686-${pkg}-any.pkg.tar.xz
        done
        export TMPDIR=/tmp
        ;;
esac

case "$ci_buildsys" in
    (autotools)
        case "$ci_variant" in
            (debug)
                # Full developer/debug build.
                set _ "$@"
                set "$@" --enable-developer --enable-tests
                # Enable optional features that are off by default
                if [ "$ci_host" != mingw ]; then
                    set "$@" --enable-containers
                    set "$@" --enable-user-session
                fi
                shift
                # The test coverage for OOM-safety is too
                # verbose to be useful on travis-ci.
                export DBUS_TEST_MALLOC_FAILURES=0
                ;;

            (reduced)
                # A smaller configuration than normal, with
                # various features disabled; this emulates
                # an older system or one that does not have
                # all the optional libraries.
                set _ "$@"
                # No LSMs (the production build has both)
                set "$@" --disable-selinux --disable-apparmor
                # No inotify (we will use dnotify)
                set "$@" --disable-inotify
                # No epoll or kqueue (we will use poll)
                set "$@" --disable-epoll --disable-kqueue
                # No special init system support
                set "$@" --disable-launchd --disable-systemd
                # No libaudit or valgrind
                set "$@" --disable-libaudit --without-valgrind
                # Disable optional features, some of which are on by
                # default
                set "$@" --disable-containers
                set "$@" --disable-stats
                set "$@" --disable-user-session
                shift
                ;;

            (legacy)
                # An unrealistically cut-down configuration,
                # to check that it compiles and works.
                set _ "$@"
                # Disable native atomic operations on Unix
                # (armv4, as used as the baseline for Debian
                # armel, is one architecture that really
                # doesn't have them)
                set "$@" dbus_cv_sync_sub_and_fetch=no
                # No epoll, kqueue or poll (we will fall back
                # to select, even on Unix where we would
                # usually at least have poll)
                set "$@" --disable-epoll --disable-kqueue
                set "$@" CPPFLAGS=-DBROKEN_POLL=1
                # Enable SELinux and AppArmor but not
                # libaudit - that configuration has sometimes
                # failed
                set "$@" --enable-selinux --enable-apparmor
                set "$@" --disable-libaudit --without-valgrind
                # No directory monitoring at all
                set "$@" --disable-inotify --disable-dnotify
                # No special init system support
                set "$@" --disable-launchd --disable-systemd
                # No X11 autolaunching
                set "$@" --disable-x11-autolaunch
                # Re-enable the deprecated pam_console support to make
                # sure it still builds
                set "$@" --with-console-auth-dir=/var/run/console
                # Leave stats, user-session, etc. at default settings
                # to check that the defaults can compile on an old OS
                shift
                ;;

            (*)
                ;;
        esac

        case "$ci_host" in
            (mingw)
                set _ "$@"
                set "$@" --build="$(build-aux/config.guess)"
                set "$@" --host=i686-w64-mingw32
                set "$@" CFLAGS=-static-libgcc
                set "$@" CXXFLAGS=-static-libgcc
                # don't run tests yet, Wine needs Xvfb and
                # more msys2 libraries
                ci_test=no
                # don't "make install" system-wide
                ci_sudo=no
                shift
                ;;
        esac

        ../configure \
            --enable-installed-tests \
            --enable-maintainer-mode \
            --enable-modular-tests \
            --with-glib \
            "$@"

        ${make}
        [ "$ci_test" = no ] || ${make} check || maybe_fail_tests
        cat test/test-suite.log || :
        [ "$ci_test" = no ] || ${make} distcheck || maybe_fail_tests

        ${make} install DESTDIR=$(pwd)/DESTDIR
        ( cd DESTDIR && find . )

        if [ "$ci_sudo" = yes ] && [ "$ci_test" = yes ]; then
            sudo ${make} install
            sudo env LD_LIBRARY_PATH=/usr/local/lib \
                /usr/local/bin/dbus-uuidgen --ensure
            LD_LIBRARY_PATH=/usr/local/lib ${make} installcheck || \
                maybe_fail_tests
            cat test/test-suite.log || :

            # re-run them with gnome-desktop-testing
            env LD_LIBRARY_PATH=/usr/local/lib \
            gnome-desktop-testing-runner -d /usr/local/share dbus/ || \
                maybe_fail_tests

            # these tests benefit from being re-run as root, and one
            # test needs a finite fd limit to be useful
            sudo env LD_LIBRARY_PATH=/usr/local/lib \
            bash -c 'ulimit -S -n 1024; ulimit -H -n 4096; exec "$@"' bash \
                gnome-desktop-testing-runner -d /usr/local/share \
                dbus/test-dbus-daemon_with_config.test \
                dbus/test-uid-permissions_with_config.test || \
                maybe_fail_tests
        fi
        ;;

    (cmake)
        case "$ci_host" in
            (mingw)
                set _ "$@"
                set "$@" -D CMAKE_TOOLCHAIN_FILE="${srcdir}/cmake/i686-w64-mingw32.cmake"
                set "$@" -D CMAKE_PREFIX_PATH="${mingw}"
                set "$@" -D CMAKE_INCLUDE_PATH="${mingw}/include"
                set "$@" -D CMAKE_LIBRARY_PATH="${mingw}/lib"
                set "$@" -D EXPAT_LIBRARY="${mingw}/lib/libexpat.dll.a"
                set "$@" -D GLIB2_LIBRARIES="${mingw}/lib/libglib-2.0.dll.a ${mingw}/lib/libgobject-2.0.dll.a ${mingw}/lib/libgio-2.0.dll.a"
                shift
                # don't run tests yet, Wine needs Xvfb and more
                # msys2 libraries
                ci_test=no
                ;;
        esac

        cmake "$@" ../cmake

        ${make}
        # The test coverage for OOM-safety is too verbose to be useful on
        # travis-ci.
        export DBUS_TEST_MALLOC_FAILURES=0
        [ "$ci_test" = no ] || ctest -VV || maybe_fail_tests
        ${make} install DESTDIR=$(pwd)/DESTDIR
        ( cd DESTDIR && find . )
        ;;
esac

# vim:set sw=4 sts=4 et:
