# arm64-osx-dynamic, overlaid on vcpkg's community triplet of the same name --
# the default on Apple Silicon (see "Dependency linkage" in BUILD.md). Same
# settings, plus the per-port SDK workarounds below. Shadowing the name rather
# than inventing one keeps the default selection in CMakeLists.txt a plain
# <arch>-<os>-dynamic on every platform.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

# c-ares probes pipe2 with check_symbol_exists, which only asks whether the SDK
# DECLARES it. The macOS 27 SDK does, but marks it available from 27.0, so on
# an older deployment target the probe passes and the compile then fails under
# c-ares's own -Werror=partial-availability. Pre-seed the probe: the
# pipe()+fcntl fallback is what every macOS before 27 runs anyway. Inlined in
# each macOS triplet rather than include()d: vcpkg hashes the triplet file
# itself, so a shared fragment could change without invalidating the cache.
if(PORT STREQUAL "c-ares")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DHAVE_PIPE2=OFF)
endif()
