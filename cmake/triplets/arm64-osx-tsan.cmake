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
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

set(VCPKG_C_FLAGS "-fsanitize=thread -fno-omit-frame-pointer")
set(VCPKG_CXX_FLAGS "-fsanitize=thread -fno-omit-frame-pointer")
set(VCPKG_LINKER_FLAGS "-fsanitize=thread")
