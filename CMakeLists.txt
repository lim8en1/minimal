cmake_minimum_required(VERSION 3.24)

# The vcpkg toolchain must be set before project(). Pass it on the command line
# (-DCMAKE_TOOLCHAIN_FILE=...) or use CMakePresets.json, which reads $VCPKG_ROOT.
project(minimal
    VERSION 0.1.0
    DESCRIPTION "Local-first coding-agent harness"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(MINIMAL_BUILD_CLIENT "Build the FTXUI TUI client" ON)
option(MINIMAL_BUILD_TESTS  "Build the test suite"       ON)

# --- Platform gate -----------------------------------------------------------
# Windows is out of scope until 2.x: the bash tool is posix_spawn/setrlimit/killpg
# and plugin loading is dlopen. WSL is the interim answer. See ARCHITECTURE.md.
if(WIN32)
    message(FATAL_ERROR
        "minimal does not support native Windows (see ARCHITECTURE.md -- Deferred). Use WSL.")
endif()

include(GNUInstallDirs)
include(FetchContent)

# Where bundled plugins are installed, and where the server looks for them
# relative to its own executable. Both halves must agree.
set(MINIMAL_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/minimal/plugins"
    CACHE STRING "Install location for bundled plugins")

# --- Plugin ABI tag ----------------------------------------------------------
# Plugins are source-compatible, not binary-compatible: they are rebuilt with the
# harness. This tag is the "you forgot to rebuild" guard, not a compatibility
# promise. Host and plugin bake the same value; a mismatch refuses to load.
set(MINIMAL_ABI_TAG
    "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-cxx${CMAKE_CXX_STANDARD}-${CMAKE_SYSTEM_NAME}")
message(STATUS "minimal plugin ABI tag: ${MINIMAL_ABI_TAG}")

# --- Dependencies ------------------------------------------------------------
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

if(MINIMAL_BUILD_CLIENT)
    find_package(ftxui CONFIG REQUIRED)
    # md4c ships as either md4c::md4c or a plain lib depending on the port version.
    find_package(md4c CONFIG QUIET)
    if(NOT md4c_FOUND)
        message(FATAL_ERROR "md4c not found; install via vcpkg or vendor it via FetchContent")
    endif()
endif()

if(MINIMAL_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    include(CTest)
    include(Catch)
endif()

# Not vcpkg-packaged: vendored directly.
FetchContent_Declare(cpp_mcp
    GIT_REPOSITORY https://github.com/hkr04/cpp-mcp.git
    GIT_TAG        main)          # TODO: pin to a release SHA before first release
FetchContent_MakeAvailable(cpp_mcp)

add_subdirectory(proto)
add_subdirectory(src/common)
add_subdirectory(src/server)
if(MINIMAL_BUILD_CLIENT)
    add_subdirectory(src/client)
endif()
add_subdirectory(src/plugins)

if(MINIMAL_BUILD_TESTS)
    add_subdirectory(tests)
endif()
