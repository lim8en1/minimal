# Drives cmake/SeedConfig.cmake against throwaway directories.
#
# CMake rather than Catch2 because the seeding is CMake -- testing it through a
# C++ reimplementation would assert that two copies of the logic agree, which is
# the bug this arrangement exists to avoid. Run as:
#
#   cmake -DSEED_SCRIPT=... -DBUNDLED_DIR=... -DWORK_DIR=... -P seed_config_test.cmake

if(NOT DEFINED SEED_SCRIPT OR NOT DEFINED BUNDLED_DIR OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "SEED_SCRIPT, BUNDLED_DIR and WORK_DIR are all required")
endif()

set(_config "${WORK_DIR}/config")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${_config}")

function(seed)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -DMINIMAL_SEED_SOURCE=${BUNDLED_DIR}
                -DMINIMAL_SEED_CONFIG_DIR=${_config}
                -P "${SEED_SCRIPT}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "seed failed (${_rc}):\n${_out}\n${_err}")
    endif()
endfunction()

function(expect_file_contains path needle)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "expected ${path} to exist")
    endif()
    file(READ "${path}" _contents)
    string(FIND "${_contents}" "${needle}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "expected ${path} to contain '${needle}', got:\n${_contents}")
    endif()
endfunction()

# --- A cold config dir gets the whole skeleton ------------------------------
# The cold-start failure this closes: LoadSettingsConfig throws on a missing
# file and main.cpp turns that into exit 1, so before seeding the server died at
# stage 4 and the user fixed one filename per run to find the next.
seed()
foreach(_needed settings.json permissions.json providers.json providers.example.json
                prompts/compression.md prompts/subagent.md agents/build.md agents/plan.md)
    if(NOT EXISTS "${_config}/${_needed}")
        message(FATAL_ERROR "cold seed did not produce ${_needed}")
    endif()
endforeach()

# providers.json is deliberately empty: a base_url that is right on the machine
# that built this is wrong on the next one. The example beside it carries the
# shape.
# Empty means "no providers", not "no file": the server must start so the
# startup sanity check can report every problem at once.
file(READ "${_config}/providers.json" _providers)
string(REGEX REPLACE "[ \t\r\n]" "" _providers "${_providers}")
if(NOT _providers STREQUAL "{\"providers\":{}}")
    message(FATAL_ERROR "providers.json should seed with no providers, got: ${_providers}")
endif()
# The example has to show the nesting, which is the part of the schema someone
# copying it needs to get right.
expect_file_contains("${_config}/providers.example.json" "\"models\"")
expect_file_contains("${_config}/providers.example.json" "<provider>/<model>")

# --- Re-seeding never overwrites --------------------------------------------
# THE INVARIANT. Silently reverting an edited compaction prompt or preset on
# every `cmake --install` would be a worse failure than shipping no default at
# all -- and an upgrade is exactly when the user is least watching.
file(WRITE "${_config}/prompts/compression.md" "MY OWN PROMPT\n")
file(WRITE "${_config}/agents/build.md" "MY OWN PRESET\n")
file(WRITE "${_config}/providers.json" "{\"providers\":{\"mine\":{}}}\n")
seed()
expect_file_contains("${_config}/prompts/compression.md" "MY OWN PROMPT")
expect_file_contains("${_config}/agents/build.md" "MY OWN PRESET")
expect_file_contains("${_config}/providers.json" "mine")

# --- Seeding is per file, not all-or-nothing --------------------------------
# A config dir that has settings.json but no agents/ still gets the presets.
file(REMOVE "${_config}/agents/plan.md")
seed()
expect_file_contains("${_config}/agents/plan.md" "name: plan")
expect_file_contains("${_config}/agents/build.md" "MY OWN PRESET")  # still theirs

# --- DESTDIR means a packaging root, not a user ------------------------------
# Staging a package must not write into the builder's home directory.
set(_untouched "${WORK_DIR}/destdir-config")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env DESTDIR=/tmp/staging
            "${CMAKE_COMMAND}"
            -DMINIMAL_SEED_SOURCE=${BUNDLED_DIR}
            -DMINIMAL_SEED_CONFIG_DIR=${_untouched}
            -P "${SEED_SCRIPT}"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "seed under DESTDIR should succeed as a no-op, got ${_rc}")
endif()
if(EXISTS "${_untouched}")
    message(FATAL_ERROR "seeding under DESTDIR wrote to ${_untouched}")
endif()

# --- A missing bundle is a warning, not a failure ----------------------------
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DMINIMAL_SEED_SOURCE=${WORK_DIR}/does-not-exist
            -DMINIMAL_SEED_CONFIG_DIR=${_config}
            -P "${SEED_SCRIPT}"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "a missing bundle should not fail the install, got ${_rc}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "seed_config: all checks passed")
