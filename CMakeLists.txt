cmake_minimum_required(VERSION 3.28)

# The vcpkg toolchain must be set before project(). Pass it on the command line
# (-DCMAKE_TOOLCHAIN_FILE=...) or use CMakePresets.json, which reads $VCPKG_ROOT.
#
# --- Dependency linkage: dynamic vcpkg triplet -------------------------------
# Every vcpkg dependency is a SHARED library, on every platform. vcpkg's default
# triplets (x64-linux, arm64-osx) are static, and a static protobuf/gRPC/absl
# reaches every shared object here -- both cores, every plugin, the tests --
# through minimal_proto's PUBLIC gRPC::grpc++. Each object then carries its own
# partial copy. On macOS the two-level namespace keeps the copies apart and
# nothing crashes; on Linux's flat namespace they share one interposed
# descriptor pool, and the second copy's static initializers abort the server
# at startup ("File already exists in database"). One shared library, one copy
# in memory, one init -- the rule minimal_proto already follows (see
# proto/CMakeLists.txt). The guard after find_package(gRPC) enforces it.
#
# Chosen here, not in the presets, because the name depends on the host and
# the toolchain reads it during project(). An explicit -DVCPKG_TARGET_TRIPLET
# (the sanitizer presets pass one) is left alone.
#
# cmake/triplets/ overlays: the sanitizer triplets, and the macOS dynamic
# triplets that shadow vcpkg's community ones to carry per-port SDK
# workarounds. Appended here, not only in the presets, so a configure without
# a preset resolves the same triplet files.
if(NOT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/triplets" IN_LIST VCPKG_OVERLAY_TRIPLETS)
    list(APPEND VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/triplets")
    set(VCPKG_OVERLAY_TRIPLETS "${VCPKG_OVERLAY_TRIPLETS}" CACHE STRING
        "vcpkg overlay triplet directories" FORCE)
endif()
# cmake/ports/ overlays: abseil, whose vcpkg port is static-only outside
# Windows. A static abseil is embedded in libprotobuf, libgrpc, libgpr, ...
# each its own copy, and on macOS a CondVar waited on in one copy is
# signalled from another -- gRPC server shutdown hangs forever. The overlay is
# the baseline port with only that restriction lifted (see its portfile);
# refresh it from the new baseline when bumping builtin-baseline.
if(NOT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ports" IN_LIST VCPKG_OVERLAY_PORTS)
    list(APPEND VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ports")
    set(VCPKG_OVERLAY_PORTS "${VCPKG_OVERLAY_PORTS}" CACHE STRING
        "vcpkg overlay port directories" FORCE)
endif()
# Clean each port's transient buildtrees/packages entries right after it
# installs, instead of letting them accumulate (~25 GiB for this manifest).
# Only the binary cache (~/.cache/vcpkg) and build/vcpkg_installed/ persist.
# Not --clean-after-build: the downloads/ tarball cache is tiny and saves a
# re-download when a failed port is retried. A failed port keeps its buildtree
# for log inspection regardless -- cleanup only happens on success. The toolchain
# normalizes this variable into the cache at project(), so append to whatever
# is there (a pre-existing build dir holds an empty cache entry from earlier
# runs) instead of requiring the entry to not exist; pass the flags yourself in
# -DVCPKG_INSTALL_OPTIONS to manage them explicitly.
if(NOT VCPKG_INSTALL_OPTIONS MATCHES "--clean-buildtrees-after-build")
    list(APPEND VCPKG_INSTALL_OPTIONS "--clean-buildtrees-after-build")
endif()
if(NOT VCPKG_INSTALL_OPTIONS MATCHES "--clean-packages-after-build")
    list(APPEND VCPKG_INSTALL_OPTIONS "--clean-packages-after-build")
endif()
cmake_host_system_information(RESULT _minimal_host_arch QUERY OS_PLATFORM)
if(_minimal_host_arch MATCHES "^(x86_64|AMD64|amd64)$")
    set(_minimal_triplet_arch x64)
elseif(_minimal_host_arch MATCHES "^(arm64|aarch64|ARM64)$")
    set(_minimal_triplet_arch arm64)
endif()
if(CMAKE_HOST_APPLE)
    set(_minimal_triplet_os osx)
elseif(CMAKE_HOST_LINUX)
    set(_minimal_triplet_os linux)
endif()
if(DEFINED _minimal_triplet_arch AND DEFINED _minimal_triplet_os)
    set(_minimal_static_triplet "${_minimal_triplet_arch}-${_minimal_triplet_os}")
    # A build tree configured before this change has the toolchain's own
    # static default cached. A plain reconfigure would keep it forever (a CACHE
    # default only applies when nothing is cached), so migrate it once. How we
    # tell "the toolchain's default" from "the user asked for static": the
    # toolchain caches it under its own helpstring, while a command-line -D
    # gets CMake's "No help..." one. Done once per tree (the marker), so a later
    # explicit -D back to static reaches the guard instead of being undone.
    get_property(_minimal_triplet_help CACHE VCPKG_TARGET_TRIPLET PROPERTY HELPSTRING)
    if(NOT MINIMAL_TRIPLET_MIGRATED
       AND VCPKG_TARGET_TRIPLET STREQUAL _minimal_static_triplet
       AND _minimal_triplet_help MATCHES "^Vcpkg target triplet")
        message(STATUS "minimal: migrating vcpkg triplet ${_minimal_static_triplet} -> "
                       "${_minimal_static_triplet}-dynamic (dependencies rebuild once)")
        set(VCPKG_TARGET_TRIPLET "${_minimal_static_triplet}-dynamic" CACHE STRING "" FORCE)
    endif()
    set(VCPKG_TARGET_TRIPLET "${_minimal_static_triplet}-dynamic" CACHE STRING
        "vcpkg target triplet (minimal requires dynamic library linkage)")
endif()
# Changing the triplet is not the whole migration: every find_package and
# find_library result of a configure that predates dynamic linkage is cached
# (protobuf_DIR, ZLIB_LIBRARY, ...) and still points into the install tree AS
# IT WAS -- the renamed default's static tree, or, for the sanitizer triplets
# (same name, linkage flipped underneath), a lib/ whose .a files vcpkg has
# since replaced with .dylib/.so. Either way the new configure would link the
# stale paths as-is. So, once per tree, drop every cached entry pointing into
# a vcpkg install tree and let everything be found afresh. tools/ is kept:
# the host triplet is still static, and protoc and grpc_cpp_plugin really do
# live in its tools/ tree. A fresh tree has nothing cached; this is a no-op.
if(NOT MINIMAL_TRIPLET_MIGRATED)
    get_cmake_property(_minimal_cache_vars CACHE_VARIABLES)
    foreach(_minimal_var IN LISTS _minimal_cache_vars)
        get_property(_minimal_val CACHE "${_minimal_var}" PROPERTY VALUE)
        if(_minimal_val MATCHES "/vcpkg_installed/[^/]+/"
           AND NOT _minimal_val MATCHES "/vcpkg_installed/[^/]+/tools/")
            unset("${_minimal_var}" CACHE)
        endif()
    endforeach()
    set(MINIMAL_TRIPLET_MIGRATED ON CACHE INTERNAL "")
endif()

project(minimal
    VERSION 1.0.1
    DESCRIPTION "Local-first coding-agent harness"
    LANGUAGES C CXX)  # C for the vendored tree-sitter grammar sources

# Beta marker appended to the user-visible version string (MINIMAL_VERSION).
# project(VERSION) only accepts numeric components, so the suffix lives here --
# library VERSION/SOVERSION stay numeric (1.0.1/1); set to "" for a final release.
set(MINIMAL_VERSION_SUFFIX "b")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ccache, if present, transparently caches object files: a no-op on a cold
# cache, a large wall-clock win on rebuilds after a branch switch or clean.
# Applied unconditionally, including to the sanitizer presets, since ccache
# hashes the full compiler invocation (including -fsanitize=...), so an asan
# and a plain build never collide in the same cache. Silently absent if
# ccache is not installed; nothing downstream depends on it.
find_program(MINIMAL_CCACHE_PROGRAM ccache)
if(MINIMAL_CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${MINIMAL_CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${MINIMAL_CCACHE_PROGRAM}")
endif()

option(MINIMAL_BUILD_CLIENT "Build the FTXUI TUI client" ON)
option(MINIMAL_BUILD_TESTS  "Build the test suite"       ON)

# Applied to our own targets only -- add_compile_options() would also hit
# FetchContent'd dependencies, whose warnings we can neither fix nor act on.
# Measured before enabling: the first-party tree was already clean apart from two
# sign-compare lines, so this costs nothing to turn on and something to leave off.
set(MINIMAL_WARNING_FLAGS -Wall -Wextra)

# Puts each function/global in its own section so the linker's dead-code
# elimination below (--gc-sections / -dead_strip) can drop ones nothing
# references -- pure object-file layout metadata, no ABI or runtime effect,
# so applied globally rather than to our own targets only. Appending (not
# a plain set()) for the same reason as the RPATH flags below: the sanitizer
# presets already populate these as cache variables.
string(APPEND CMAKE_C_FLAGS " -ffunction-sections -fdata-sections")
string(APPEND CMAKE_CXX_FLAGS " -ffunction-sections -fdata-sections")

# Old-style DT_RPATH on Linux, not DT_RUNPATH: glibc consults DT_RPATH (of the
# loaded object and its loaders) for a dependency's OWN dependencies, but
# DT_RUNPATH only for the object's direct ones. The gathered libraries
# (lib/minimal/runtime) carry no RUNPATH of their own, so under new-dtags
# libgrpc++.so.54's NEEDED libprotobuf.so.53 would fall off our RPATH chain and
# only the ld.so cache could save it. Appending preserves what the configure
# preset or the caller passed (the sanitizer presets put -fsanitize here).
# macOS has no such tag -- its linker would reject the flag -- so the gate.
if(NOT APPLE)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--disable-new-dtags")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--disable-new-dtags")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS " -Wl,--disable-new-dtags")
endif()

# Same dead-code elimination at link time, flag name split by linker: GNU ld
# and lld (Linux) use --gc-sections; ld64 (macOS) uses -dead_strip, which it
# does NOT enable by default for any Mach-O output type (executable, dylib,
# or a plugin bundle) -- that's Xcode's "Dead Code Stripping" build setting
# opting in, and nothing here goes through Xcode. CMAKE_MODULE_LINKER_FLAGS
# matters because the bundled plugins build as MODULE targets.
if(APPLE)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,-dead_strip")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,-dead_strip")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS " -Wl,-dead_strip")
else()
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--gc-sections")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--gc-sections")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS " -Wl,--gc-sections")
endif()

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
#
# .../plugins/server, a SIBLING of the client tree below, not its parent: the
# server's bundled scan (plugin_discovery.cpp) recurses into MINIMAL_PLUGIN_DIR
# to support the build tree's one-subdirectory-per-plugin layout, so an
# installed server whose plugin dir CONTAINED plugins/client/ would walk into
# it, find client plugins missing the server's required ABI symbols, and abort
# startup with them reported Fatal (bundled provenance). Only the build tree
# (where the two are already siblings under build/plugins/) escaped this,
# which is why no test caught it.
# A build tree configured before the server/client split above cached the
# old, pre-split default (no /server suffix). A plain reconfigure of that
# tree would otherwise keep serving the stale value forever -- CACHE STRING
# only takes the new default when nothing is already cached -- reproducing
# the exact bundled-scan-walks-into-client-plugins startup abort this split
# exists to fix. Force it back onto the new default in that one case; a
# value the operator set to anything else is left untouched.
if(DEFINED MINIMAL_PLUGIN_INSTALL_DIR AND
   MINIMAL_PLUGIN_INSTALL_DIR STREQUAL "${CMAKE_INSTALL_LIBDIR}/minimal/plugins")
    set(MINIMAL_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/minimal/plugins/server"
        CACHE STRING "Install location for bundled server plugins" FORCE)
endif()
set(MINIMAL_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/minimal/plugins/server"
    CACHE STRING "Install location for bundled server plugins")

# The client plugin tree, separate from the server's for the same reason the
# loaders are separate: different ABI symbols, different discovery file. The
# client resolves it relative to its own executable (bundled_assets).
set(MINIMAL_CLIENT_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/minimal/plugins/client"
    CACHE STRING "Install location for bundled client plugins")

# Where the GATHERED third-party shared libraries install (the
# minimal_runtime_deps set: libcurl, libgrpc, libssl, ...). Under lib/, but
# never IN it: ldconfig indexes <prefix>/lib itself (Debian/Ubuntu's libc.conf
# lists it) without recursing into subdirectories, so a library whose soname
# collides with a system one -- libcurl.so.4, the name every distro binary
# requests -- would win the ld.so cache and be loaded by EVERY program on the
# machine. With vcpkg's curl built without symbol versioning that surfaces as
# "/usr/local/lib/libcurl.so.4: no version information available" from any
# distro curl consumer, and the worse part is silent: that program now runs
# against vcpkg's curl and OpenSSL instead of the OS's. From
# lib/minimal/runtime ldconfig never sees them, and the RPATH hops computed in
# cmake/MinimalPlugins.cmake are what resolve them for our binaries. Our own
# cores stay in lib/ proper -- no distro ships libminimal_*.
set(MINIMAL_RUNTIME_DEP_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/minimal/runtime"
    CACHE STRING "Install location for the gathered third-party runtime libraries")

# Split debug info (.debug / .dSYM, see cmake/StripDebugInfo.cmake) installs
# here rather than alongside its binary, for the same class of reason
# MINIMAL_RUNTIME_DEP_INSTALL_DIR sits outside lib/: the bundled plugin scan
# (plugin_discovery.cpp) walks MINIMAL_PLUGIN_INSTALL_DIR with a RECURSIVE
# directory iterator, and a macOS .dSYM bundle contains its own copy of the
# binary at the identical filename one level down
# (foo.so.dSYM/Contents/Resources/DWARF/foo.so) -- colocating it inside the
# plugin dir makes every plugin scan as "found more than once". Mirrored
# under this dir by each target's own dest_subdir instead, so it never sits
# inside a directory anything else in the tree scans.
set(MINIMAL_DEBUG_INFO_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/minimal/debug"
    CACHE STRING "Install location for split-out debug info")

# Bundled + user plugin construction helpers (minimal_add_plugin,
# minimal_add_client_plugin, minimal_add_user_plugin). Included here so the
# functions are visible to every add_subdirectory below, including the
# user-plugins/ tree registered at the bottom of this file.
include(cmake/MinimalPlugins.cmake)

# Install-time debug-symbol splitting (minimal_strip_debug_info). Included
# here, alongside the plugin helpers, so it's visible to every
# add_subdirectory below.
include(cmake/StripDebugInfo.cmake)

# bundled/ is copied into the USER'S CONFIG DIR at install time, not installed
# under the prefix and read from there. Everything in it -- prompts, presets,
# settings -- is a file the user owns and edits; a copy sitting in the install
# tree would either shadow theirs or be dead weight. So there is no
# MINIMAL_PROMPT_INSTALL_DIR and no runtime lookup: one copy, in the config dir,
# and that is the one that runs. Never overwrites. See cmake/SeedConfig.cmake.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SeedConfig.cmake"
               "${CMAKE_CURRENT_BINARY_DIR}/SeedConfig.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/SeedConfig.cmake")
# The same script's other modes, on demand: `check-config` reports which seeded
# files this release changed, `update-config` replaces the ones the user never
# touched and writes <file>.bundled-new beside the ones they did.
foreach(_seed_mode IN ITEMS check update)
    add_custom_target(${_seed_mode}-config
        COMMAND "${CMAKE_COMMAND}" -DMINIMAL_SEED_MODE=${_seed_mode}
                -P "${CMAKE_CURRENT_BINARY_DIR}/SeedConfig.cmake"
        COMMENT "minimal: ${_seed_mode} the config dir against bundled/"
        USES_TERMINAL VERBATIM)
endforeach()

# --- Plugin ABI tag ----------------------------------------------------------
# Plugins are source-compatible, not binary-compatible: they are rebuilt with the
# harness. This tag is the "you forgot to rebuild" guard, not a compatibility
# promise. Host and plugin bake the same value; a mismatch refuses to load.
#
# The toolchain fields alone do not catch a stale-but-same-compiler .so: two
# builds of this tree at different commits, same compiler, produce identical
# tags and the stale plugin loads and reads the wrong struct/vtable offsets.
# The git revision closes that gap for the common case (a real ./configure &&
# make build after a pull) -- it does not catch uncommitted local edits
# rebuilt without a commit, which is why plugin_loader.cpp additionally
# compares minimal_plugin_version() against the host's own version.
find_package(Git QUIET)
set(MINIMAL_GIT_REVISION "nogit")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE MINIMAL_GIT_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE MINIMAL_GIT_REVISION_RESULT)
    if(NOT MINIMAL_GIT_REVISION_RESULT EQUAL 0 OR MINIMAL_GIT_REVISION STREQUAL "")
        # NOT a shared "unknown": two builds from different sources would
        # both tag "unknown", and a stale plugin from one tree would load
        # into the other. A configure timestamp makes every build tree
        # distinct -- the fail-closed direction (plugins rebuild, always).
        string(TIMESTAMP MINIMAL_GIT_REVISION "nogit-%Y%m%d%H%M%S")
    endif()
    # A checkout/commit changes .git/HEAD (and, on a branch, the ref file it
    # points at) without touching any CMakeLists.txt -- without this, ninja
    # would happily rebuild against a stale cached MINIMAL_GIT_REVISION.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --git-path HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE MINIMAL_GIT_HEAD_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} symbolic-ref -q HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE MINIMAL_GIT_SYMBOLIC_REF
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT MINIMAL_GIT_SYMBOLIC_REF STREQUAL "")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-parse --git-path "${MINIMAL_GIT_SYMBOLIC_REF}"
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE MINIMAL_GIT_REF_PATH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(EXISTS "${MINIMAL_GIT_REF_PATH}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                             "${MINIMAL_GIT_REF_PATH}")
            endif()
        endif()
    endif()
endif()
set(MINIMAL_ABI_TAG
    "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-cxx${CMAKE_CXX_STANDARD}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_BUILD_TYPE}-${MINIMAL_GIT_REVISION}")
message(STATUS "minimal plugin ABI tag: ${MINIMAL_ABI_TAG}")

# --- Dependencies ------------------------------------------------------------
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# Fail closed on a static protobuf, gRPC or abseil, whatever supplied it (see
# the triplet selection and the abseil overlay above project() for what a
# duplicated copy does). absl::base arrives with protobuf's config. Checks the
# imported targets, not the triplet name, so a Homebrew or system package
# build -- shared either way -- passes without special cases.
foreach(_minimal_dep IN ITEMS protobuf::libprotobuf gRPC::grpc++ absl::base)
    get_target_property(_minimal_dep_type ${_minimal_dep} TYPE)
    if(_minimal_dep_type STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR
            "minimal: ${_minimal_dep} is a static library (vcpkg triplet: "
            "'${VCPKG_TARGET_TRIPLET}'). Every shared library linking it would carry "
            "its own copy: protobuf's copies collide at server startup on Linux "
            "(\"File already exists in database\"), abseil's deadlock gRPC shutdown "
            "on macOS. Use a dynamic triplet -- the default, <arch>-<os>-dynamic, "
            "when VCPKG_TARGET_TRIPLET is not set -- with the cmake/ports overlays. "
            "See BUILD.md, \"Dependency linkage\".")
    endif()
endforeach()

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
# FindSQLite3 names the target SQLite3::SQLite3 only since CMake 4.3; older
# CMake (3.28 is the minimum above -- Ubuntu 24.04 ships it) defines just the
# now-deprecated SQLite::SQLite3. The alias is the compatibility shim the
# module's own documentation gives; visible here and in every subdirectory.
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()
find_package(nlohmann_json CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# tree-sitter core is a BASE dependency: the tools-bash plugin's command
# scanner uses it, and bundled plugins are always built (the client's future
# highlighting is a second consumer, not the first). Two config names because
# vcpkg exports `unofficial-tree-sitter` while brew exports `tree-sitter`;
# normalized behind one interface target so consumers never see the difference.
find_package(unofficial-tree-sitter CONFIG QUIET)
if(unofficial-tree-sitter_FOUND)
    add_library(minimal_tree_sitter INTERFACE)
    target_link_libraries(minimal_tree_sitter INTERFACE unofficial::tree-sitter::tree-sitter)
else()
    find_package(tree-sitter CONFIG REQUIRED)
    add_library(minimal_tree_sitter INTERFACE)
    target_link_libraries(minimal_tree_sitter INTERFACE tree-sitter::tree-sitter)
endif()

# RE2 backs the tools-search plugin's regex matching. The point of the
# dependency is the LINEAR-TIME guarantee: grep patterns are model-authored,
# and a backtracking engine would let `(a+)+b` wedge the session worker thread
# -- the exact hang bash's timeout_ms exists to bound, now unreachable.
find_package(re2 CONFIG REQUIRED)
add_library(minimal_re2 INTERFACE)
target_link_libraries(minimal_re2 INTERFACE re2::re2)

if(MINIMAL_BUILD_CLIENT)
    find_package(ftxui CONFIG REQUIRED)
    find_package(md4c CONFIG REQUIRED)   # exports md4c::md4c (verified: vcpkg + brew agree)
endif()

if(MINIMAL_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    include(CTest)
    include(Catch)
endif()

# The one vendored dependency outside the grammar set, and the first
# tree-sitter GRAMMAR: the
# registry packages the core runtime but not the per-language parsers (see
# ARCHITECTURE.md -- Dependency management). Grammars ship a generated
# parser.c, so vendoring means compiling two C files, not running the
# tree-sitter CLI/Node toolchain.
#
# Pinned to a tag: this repo publishes releases. SOURCE_SUBDIR
# points at a directory that does not exist, which stops MakeAvailable from
# add_subdirectory-ing the grammar's own CMakeLists -- that file is written for
# standalone distribution (BUILD_SHARED_LIBS ON, a tree-sitter-CLI regeneration
# rule) and we want neither. We compile the two C files ourselves below.
FetchContent_Declare(tree_sitter_bash
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-bash.git
    GIT_TAG        v0.25.1  # 2026-08; ABI 14/15, loads into tree-sitter core >= 0.24
    SOURCE_SUBDIR  cmake/no-upstream-build)
FetchContent_MakeAvailable(tree_sitter_bash)

# No MINIMAL_WARNING_FLAGS here, deliberately: this is third-party generated C
# (a ~10 MB parser table), compiled with default flags, outside our warning
# set. PIC because it links into plugin
# MODULEs.
add_library(minimal_tree_sitter_bash STATIC
    "${tree_sitter_bash_SOURCE_DIR}/src/parser.c"
    "${tree_sitter_bash_SOURCE_DIR}/src/scanner.c")
target_include_directories(minimal_tree_sitter_bash
    PUBLIC "${tree_sitter_bash_SOURCE_DIR}/src")
set_target_properties(minimal_tree_sitter_bash PROPERTIES
    C_STANDARD 11
    POSITION_INDEPENDENT_CODE ON)

add_subdirectory(proto)
add_subdirectory(src/common)
add_subdirectory(src/server)
if(MINIMAL_BUILD_CLIENT)
    add_subdirectory(src/client)
endif()
# Bundled plugin implementations. A top-level peer of src/, not part of it --
# they are artifacts the harness loads, not harness code. See plugins/CMakeLists.txt.
add_subdirectory(plugins)

if(MINIMAL_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# --- User plugins -------------------------------------------------------------
#
# user-plugins/{server,client}/<name> -- one plugin directory per entry: a
# real directory, a symlink into your own plugin repo, or a git clone. The
# directory is gitignored: this is machine-local wiring, not harness content.
#
# The server/client SPLIT is the subtree the entry sits in, never anything the
# plugin itself declares -- a plugin's CMakeLists.txt is the same one-liner on
# both sides (minimal_add_user_plugin), and a repo holding both kinds
# registers one symlink per plugin. Missing dir or empty glob: fine; new
# entries are picked up by the next build (CONFIGURE_DEPENDS re-globs).
#
# A sibling `<name>.skip` file parks an entry without unlinking it: the plugin
# is not configured or built (an unmaintained plugin whose sources lag the
# harness). Delete the file to build it again.
#
# After add_subdirectory(tests) so a user plugin's own tests can link
# minimal::test_support when it exists.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/user-plugins")
    foreach(_kind IN ITEMS server client)
        file(GLOB _user_plugin_entries LIST_DIRECTORIES true CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/user-plugins/${_kind}/*")
        if(_kind STREQUAL "client" AND NOT MINIMAL_BUILD_CLIENT AND _user_plugin_entries)
            message(STATUS
                "minimal: MINIMAL_BUILD_CLIENT=OFF; skipping user client plugins "
                "under user-plugins/client/")
        elseif(_user_plugin_entries)
            foreach(_entry IN LISTS _user_plugin_entries)
                if(NOT EXISTS "${_entry}/CMakeLists.txt")
                    continue()  # a stray file, or a repo root that is not a plugin
                endif()
                cmake_path(GET _entry FILENAME _entry_name)
                if(EXISTS "${_entry}.skip")
                    message(STATUS "minimal: user plugin '${_entry_name}' parked (${_entry_name}.skip); not built")
                    continue()
                endif()
                set(MINIMAL_USER_PLUGIN_KIND "${_kind}")
                add_subdirectory("${_entry}" "user-plugins/${_kind}/${_entry_name}")
                unset(MINIMAL_USER_PLUGIN_KIND)
            endforeach()
        endif()
        unset(_user_plugin_entries)
    endforeach()
endif()

# --- Runtime dependencies ------------------------------------------------------
#
# The dependencies are shared libraries (see the triplet selection at the top),
# so an installed tree needs them available to the binaries, cores and plugins.
# They install into MINIMAL_RUNTIME_DEP_INSTALL_DIR (lib/minimal/runtime), NOT
# into lib/ itself: ldconfig indexes lib/ non-recursively, and a soname that
# collides with a system library there would be loaded by every program on the
# machine (the libcurl.so.4 case -- see the variable's comment above). The
# binaries', cores' and plugins' RPATHs carry an extra hop to the runtime dir,
# computed in cmake/MinimalPlugins.cmake and applied next to each install().
# Every install(TARGETS) above joins the minimal_runtime_deps set; this
# installs what the set resolves to. Only files from the vcpkg installed tree
# ship: system libraries belong to the OS, and our own libminimal_* are
# installed by their install(TARGETS). Without vcpkg (Homebrew, system
# packages) nothing matches the include, so nothing is copied -- the package
# manager owns those.
set(_minimal_runtime_dep_dirs "")
set(_minimal_runtime_dep_include "")
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_minimal_vcpkg_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_minimal_runtime_dep_dirs "${_minimal_vcpkg_prefix}/debug/lib")
    else()
        set(_minimal_runtime_dep_dirs "${_minimal_vcpkg_prefix}/lib")
    endif()
    string(REGEX REPLACE "([][+.*?()^$|\\\\{}])" "\\\\\\1"
           _minimal_runtime_dep_include "${_minimal_vcpkg_prefix}")
    set(_minimal_runtime_dep_include "^${_minimal_runtime_dep_include}/")
endif()
install(RUNTIME_DEPENDENCY_SET minimal_runtime_deps
    DESTINATION "${MINIMAL_RUNTIME_DEP_INSTALL_DIR}"
    DIRECTORIES ${_minimal_runtime_dep_dirs}
    # macOS system libraries live in the dyld shared cache, not on disk, so
    # they cannot resolve; they are the OS's regardless. (Linux sonames never
    # start with '/', so this is a no-op there.)
    PRE_EXCLUDE_REGEXES "^/usr/lib/" "^/System/"
    POST_INCLUDE_REGEXES ${_minimal_runtime_dep_include}
    POST_EXCLUDE_REGEXES ".*")

# The dependency copy emits the versioned FILE only (libcurl.so.4.8.0), but on
# Linux our objects record DT_NEEDED as the SONAME (libcurl.so.4), so the link
# needs the soname symlink sitting beside it. cmake/InstallSonameLinks.cmake
# creates it at install time; on macOS it is a no-op (LC_LOAD_DYLIB records
# the full versioned name, which is exactly what gets copied).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/InstallSonameLinks.cmake")
minimal_install_soname_links("${MINIMAL_RUNTIME_DEP_INSTALL_DIR}")
