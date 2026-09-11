# x64-osx-dynamic, overlaid on vcpkg's community triplet of the same name --
# the default on Intel Macs. The twin of arm64-osx-dynamic.cmake; see it.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)

# c-ares vs. a newer SDK's pipe2 declaration -- see arm64-osx-dynamic.cmake.
if(PORT STREQUAL "c-ares")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DHAVE_PIPE2=OFF)
endif()
