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

# A private copy of the bundle: the update checks below play "the next release"
# by editing it.
set(_bundle "${WORK_DIR}/bundled")
file(COPY "${BUNDLED_DIR}/" DESTINATION "${_bundle}")

# seed([mode]) -- mode defaults to the install step's "seed". The script's
# output lands in SEED_OUTPUT in the caller's scope.
function(seed)
    set(_mode seed)
    if(ARGC GREATER 0)
        set(_mode "${ARGV0}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -DMINIMAL_SEED_SOURCE=${_bundle}
                -DMINIMAL_SEED_CONFIG_DIR=${_config}
                -DMINIMAL_SEED_MODE=${_mode}
                -P "${SEED_SCRIPT}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "seed ${_mode} failed (${_rc}):\n${_out}\n${_err}")
    endif()
    set(SEED_OUTPUT "${_out}${_err}" PARENT_SCOPE)
endfunction()

function(expect_output_contains needle)
    string(FIND "${SEED_OUTPUT}" "${needle}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "expected output to contain '${needle}', got:\n${SEED_OUTPUT}")
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
                prompts/compression.md prompts/subagent.md agents/build.md agents/plan.md
                commands/.keep)
    if(NOT EXISTS "${_config}/${_needed}")
        message(FATAL_ERROR "cold seed did not produce ${_needed}")
    endif()
endforeach()

# commands-md's global (trusted) discovery dir must exist after a cold seed --
# otherwise it is never created by anything, and a user dropping a *.md file
# into it has to `mkdir` by hand first with no prompt telling them so.
if(NOT IS_DIRECTORY "${_config}/commands")
    message(FATAL_ERROR "cold seed did not create the commands/ directory")
endif()

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

# --- The seed record -------------------------------------------------------
# Every seeded file's hash is recorded: it is what later proves "untouched".
expect_file_contains("${_config}/.bundled.sha256" "  agents/plan.md\n")
expect_file_contains("${_config}/.bundled.sha256" "  settings.json\n")

# --- The next release: check reports, writes nothing ------------------------
# review.md untouched by the user, build.md edited (above) -- both change
# upstream. compression.md is edited but NOT changed upstream: nothing to offer.
file(APPEND "${_bundle}/agents/review.md" "\nNEXT RELEASE REVIEW\n")
file(APPEND "${_bundle}/agents/build.md" "\nNEXT RELEASE BUILD\n")
file(READ "${_config}/.bundled.sha256" _record_before)
seed(check)
expect_output_contains("stale    agents/review.md")
expect_output_contains("changed  agents/build.md")
expect_output_contains("update-config")
string(FIND "${SEED_OUTPUT}" "prompts/compression.md" _at)
if(NOT _at EQUAL -1)
    message(FATAL_ERROR "an edit the release did not touch must not be reported:\n${SEED_OUTPUT}")
endif()
file(READ "${_config}/.bundled.sha256" _record_after)
if(NOT _record_before STREQUAL _record_after)
    message(FATAL_ERROR "check mode rewrote the seed record")
endif()
if(EXISTS "${_config}/agents/build.md.bundled-new")
    message(FATAL_ERROR "check mode wrote a .bundled-new")
endif()

# --- The install step reports but still never overwrites --------------------
seed()
expect_output_contains("stale    agents/review.md")
file(READ "${_config}/agents/review.md" _review)
string(FIND "${_review}" "NEXT RELEASE REVIEW" _at)
if(NOT _at EQUAL -1)
    message(FATAL_ERROR "the install step replaced an existing file")
endif()

# --- update: untouched -> replaced; edited -> .bundled-new beside it ---------
seed(update)
expect_file_contains("${_config}/agents/review.md" "NEXT RELEASE REVIEW")
expect_file_contains("${_config}/agents/build.md" "MY OWN PRESET")
expect_file_contains("${_config}/agents/build.md.bundled-new" "NEXT RELEASE BUILD")
expect_file_contains("${_config}/prompts/compression.md" "MY OWN PROMPT")
if(EXISTS "${_config}/prompts/compression.md.bundled-new")
    message(FATAL_ERROR "offered an update for a file the release did not change")
endif()
# providers.json is a placeholder, never a default worth offering.
if(EXISTS "${_config}/providers.json.bundled-new")
    message(FATAL_ERROR "offered the empty providers.json skeleton as an update")
endif()

# Offered once: a second update is quiet until the release changes it again.
file(REMOVE "${_config}/agents/build.md.bundled-new")
seed(update)
if(EXISTS "${_config}/agents/build.md.bundled-new")
    message(FATAL_ERROR "re-offered an update the user already received")
endif()
expect_output_contains("0 stale default(s), 0 edited file(s)")

# --- A config dir from before the record ------------------------------------
# No baseline: an identical file is adopted silently, a differing one is offered.
file(REMOVE "${_config}/.bundled.sha256")
file(WRITE "${_config}/themes/dark.json" "{}\n")
seed(update)
expect_file_contains("${_config}/themes/dark.json.bundled-new" "\"")
expect_file_contains("${_config}/themes/dark.json" "{}")
expect_file_contains("${_config}/.bundled.sha256" "  agents/plan.md\n")

# --- A file the release stopped shipping is reported, never deleted ---------
file(REMOVE "${_bundle}/agents/helper.md")
seed(update)
expect_output_contains("retired  agents/helper.md")
if(NOT EXISTS "${_config}/agents/helper.md")
    message(FATAL_ERROR "a retired bundled file was deleted")
endif()

# --- DESTDIR means a packaging root, not a user ------------------------------
# Staging a package must not write into the builder's home directory.
set(_untouched "${WORK_DIR}/destdir-config")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env DESTDIR=/tmp/staging
            "${CMAKE_COMMAND}"
            -DMINIMAL_SEED_SOURCE=${_bundle}
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
