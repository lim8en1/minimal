# arm64-osx with ThreadSanitizer applied to the DEPENDENCIES as well as
# first-party code. TSan's happens-before analysis only covers translation
# units it compiled: an instrumented first-party tree against uninstrumented
# gRPC or protobuf cannot see synchronisation that happens inside them, which
# produces false negatives -- races reported nowhere rather than reported
# wrong. Same sanitizer everywhere is the only clean answer. The triplet name
# is load-bearing: vcpkg keys its binary cache and VCPKG_INSTALLED_DIR on it,
# so this preset's dependencies install beside (never over) the uninstrumented
# ones. Copied from arm64-osx-asan.cmake per that file's instruction; any new
# sanitizer preset should do the same under its own triplet name.
# Dynamic library linkage, like the default arm64-osx-dynamic triplet: a static
# protobuf/gRPC is duplicated into every core and plugin, and the top-level
# CMakeLists refuses it (see "Dependency linkage" in BUILD.md).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

set(VCPKG_C_FLAGS "-fsanitize=thread -fno-omit-frame-pointer")
set(VCPKG_CXX_FLAGS "-fsanitize=thread -fno-omit-frame-pointer")
set(VCPKG_LINKER_FLAGS "-fsanitize=thread")

# c-ares vs. a newer SDK's pipe2 declaration -- see arm64-osx-dynamic.cmake.
if(PORT STREQUAL "c-ares")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DHAVE_PIPE2=OFF)
endif()
