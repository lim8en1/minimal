# arm64-osx with AddressSanitizer applied to the DEPENDENCIES as well as
# first-party code. Instrumenting only the main tree against uninstrumented
# gRPC produces a use-after-poison in send_goaway on every server-shutdown
# path -- a stack-poison mismatch between instrumented and uninstrumented
# translation units, not a defect in either. Same sanitizer everywhere is the
# only clean answer. The triplet name is load-bearing: vcpkg keys its binary
# cache and VCPKG_INSTALLED_DIR on it, so this preset's dependencies install
# beside (never over) the uninstrumented ones. Any new sanitizer preset should
# copy this file under its own triplet name for the same reason.
# Dynamic library linkage, like the default arm64-osx-dynamic triplet: a static
# protobuf/gRPC is duplicated into every core and plugin, and the top-level
# CMakeLists refuses it (see "Dependency linkage" in BUILD.md).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

set(VCPKG_C_FLAGS "-fsanitize=address -fno-omit-frame-pointer")
set(VCPKG_CXX_FLAGS "-fsanitize=address -fno-omit-frame-pointer")
set(VCPKG_LINKER_FLAGS "-fsanitize=address")

# c-ares vs. a newer SDK's pipe2 declaration -- see arm64-osx-dynamic.cmake.
if(PORT STREQUAL "c-ares")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DHAVE_PIPE2=OFF)
endif()
