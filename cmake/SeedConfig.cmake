# Seeds the user's config dir from bundled/, at `cmake --install` time.
#
# WHY INSTALL TIME, and not first server run. Everything under bundled/ is a file
# the USER owns once it lands -- prompts, presets, settings. Reading it from the
# install prefix at runtime, or compiling it into the binary, would make the
# harness's behaviour depend on files nobody can see or edit, which is the exact
# opposite of this project's premise. Copying at install time means there is
# exactly one copy of each of these, it lives in the config dir, and it is the one
# that runs. No runtime lookup path, no build-tree/installed-tree probe, no
# precedence rule to remember.
#
# NEVER OVERWRITES. An existing file is the user's, edited or not. Silently
# reverting someone's compaction prompt or preset on every upgrade would be a far
# worse failure than shipping without a default -- so a bundled file that gains a
# better default in a later release does NOT propagate to anyone who already has
# one. That is the deliberate trade; see ARCHITECTURE.md "Bundled assets".
#
# Invoked two ways:
#   install(SCRIPT ...)                       -- the real path, during install
#   cmake -DMINIMAL_SEED_SOURCE=... -P ...    -- directly, which is how it is tested

if(NOT DEFINED MINIMAL_SEED_SOURCE)
    set(MINIMAL_SEED_SOURCE "@CMAKE_SOURCE_DIR@/bundled")
endif()

# DESTDIR means a packaging root, not a live machine: there is no user whose
# config dir this is. Staging a package must not write into the builder's home.
if(DEFINED ENV{DESTDIR} AND NOT "$ENV{DESTDIR}" STREQUAL "")
    message(STATUS "minimal: DESTDIR set, skipping config seeding "
                   "(run the server once on the target machine, or seed by hand)")
    return()
endif()

# The same resolution order as common::ConfigDir(). Kept in step by hand, which
# is a real cost -- but the alternative is running the binary to ask it, and the
# binary may not be installed yet.
if(DEFINED MINIMAL_SEED_CONFIG_DIR)
    set(_config_dir "${MINIMAL_SEED_CONFIG_DIR}")   # test seam
elseif(DEFINED ENV{MINIMAL_CONFIG_DIR} AND NOT "$ENV{MINIMAL_CONFIG_DIR}" STREQUAL "")
    set(_config_dir "$ENV{MINIMAL_CONFIG_DIR}")
elseif(DEFINED ENV{XDG_CONFIG_HOME} AND NOT "$ENV{XDG_CONFIG_HOME}" STREQUAL "")
    set(_config_dir "$ENV{XDG_CONFIG_HOME}/minimal")
elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    set(_config_dir "$ENV{HOME}/.config/minimal")
else()
    message(WARNING "minimal: no HOME or XDG_CONFIG_HOME; skipping config seeding")
    return()
endif()

if(NOT IS_DIRECTORY "${MINIMAL_SEED_SOURCE}")
    message(WARNING "minimal: no bundled config at ${MINIMAL_SEED_SOURCE}; nothing seeded")
    return()
endif()

message(STATUS "minimal: seeding config into ${_config_dir}")

file(GLOB_RECURSE _bundled RELATIVE "${MINIMAL_SEED_SOURCE}" "${MINIMAL_SEED_SOURCE}/*")
set(_seeded 0)
set(_kept 0)
foreach(_rel IN LISTS _bundled)
    set(_src "${MINIMAL_SEED_SOURCE}/${_rel}")
    set(_dst "${_config_dir}/${_rel}")
    if(IS_DIRECTORY "${_src}")
        continue()
    endif()
    if(EXISTS "${_dst}")
        # Per file, not all-or-nothing: a config dir that has settings.json but
        # not agents/ still gets the presets.
        message(STATUS "  kept   ${_rel} (yours)")
        math(EXPR _kept "${_kept} + 1")
    else()
        get_filename_component(_dst_dir "${_dst}" DIRECTORY)
        file(MAKE_DIRECTORY "${_dst_dir}")
        file(COPY "${_src}" DESTINATION "${_dst_dir}")
        message(STATUS "  seeded ${_rel}")
        math(EXPR _seeded "${_seeded} + 1")
    endif()
endforeach()

message(STATUS "minimal: ${_seeded} file(s) seeded, ${_kept} left alone")
if(NOT EXISTS "${_config_dir}/providers.json"
   OR NOT EXISTS "${_config_dir}/providers.example.json")
    return()
endif()
file(READ "${_config_dir}/providers.json" _providers)
string(REGEX REPLACE "[ \t\r\n]" "" _providers "${_providers}")
if(_providers STREQUAL "{\"providers\":{}}")
    # The one thing seeding cannot do for you. A base_url that is right on the
    # machine that built this is wrong on the next one, so there is no honest
    # default -- an empty list that plainly needs filling in beats a plausible
    # entry that silently points at nothing.
    message(STATUS "")
    message(STATUS "  minimal: no model configured yet. Edit")
    message(STATUS "    ${_config_dir}/providers.json")
    message(STATUS "  using providers.example.json beside it as a starting point.")
endif()
