# Thin wrapper over CMakePresets.json, which owns the real configuration
# (generator, build type, vcpkg toolchain). Each target below just invokes a
# preset; see BUILD.md for what the presets do.
#
# `install` installs the `default` (RelWithDebInfo) build from build/. If you
# built with another preset, point CMAKE_INSTALL_PREFIX at that preset's
# binaryDir instead (build-debug, build-server, ...).
#
# DESTDIR is honored by `cmake --install` for staged packaging. Note: a non-empty
# DESTDIR also makes cmake/SeedConfig.cmake SKIP config seeding (there is no
# user whose config dir a staged package should write into) -- seed by running
# the server once on the target machine, or by hand.

PREFIX ?= /usr/local

.PHONY: build install test clean debug server-only asan tsan debug-test asan-test tsan-test

build:
	cmake --preset default
	cmake --build --preset default

install:
	cmake --install build --prefix $(PREFIX)

test:
	cmake --preset default
	ctest --preset default

debug:
	cmake --preset debug
	cmake --build --preset debug

server-only:
	cmake --preset server-only
	cmake --build --preset server-only

asan:
	cmake --preset asan
	cmake --build --preset asan

tsan:
	cmake --preset tsan
	cmake --build --preset tsan

debug-test:
	cmake --preset debug
	ctest --preset debug

asan-test:
	cmake --preset asan
	ctest --preset asan

tsan-test:
	cmake --preset tsan
	ctest --preset tsan

clean:
	rm -rf build build-debug build-server build-asan build-tsan
