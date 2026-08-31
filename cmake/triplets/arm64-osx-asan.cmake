# arm64-osx with AddressSanitizer applied to the DEPENDENCIES as well as
# first-party code. Instrumenting only the main tree against uninstrumented
# gRPC produces a use-after-poison in send_goaway on every server-shutdown
# path -- a stack-poison mismatch between instrumented and uninstrumented
# translation units, not a defect in either. Same sanitizer everywhere is the
# only clean answer. The triplet name is load-bearing: vcpkg keys its binary
# cache and VCPKG_INSTALLED_DIR on it, so this preset's dependencies install
# beside (never over) the uninstrumented ones. Any new sanitizer preset should
# copy this file under its own triplet name for the same reason.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

set(VCPKG_C_FLAGS "-fsanitize=address -fno-omit-frame-pointer")
set(VCPKG_CXX_FLAGS "-fsanitize=address -fno-omit-frame-pointer")
set(VCPKG_LINKER_FLAGS "-fsanitize=address")
