# Tests for cmake/DeployUserPlugin.cmake -- the POST_BUILD deploy step of
# minimal_add_user_plugin. The deploy step is CMake, so its test is CMake
# (same reasoning as seed_config_test.cmake: reimplementing the copy in C++
# would assert that two copies of the logic agree).
#
# Invoked via add_test with:
#   -DDEPLOY_SCRIPT=<repo>/cmake/DeployUserPlugin.cmake
#   -DWORK_DIR=<tests binary dir>/deploy_user_plugin_work

if(NOT DEFINED DEPLOY_SCRIPT OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "deploy_user_plugin_test: DEPLOY_SCRIPT and WORK_DIR are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/src")

function(deploy_run expected_result)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_FILE=${WORK_DIR}/src/hello.so"
                "-DDEPLOY_DIR=${WORK_DIR}/dst"
                -P "${DEPLOY_SCRIPT}"
        RESULT_VARIABLE _rv
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _rv EQUAL expected_result)
        message(FATAL_ERROR
            "deploy exit ${_rv}, expected ${expected_result}\n${_out}${_err}")
    endif()
    set(_deploy_output "${_out}${_err}" PARENT_SCOPE)
endfunction()# --- 1. Fresh deploy: creates the destination dir and lands the artifact -----

file(WRITE "${WORK_DIR}/src/hello.so" "build-one")
deploy_run(0)
if(NOT EXISTS "${WORK_DIR}/dst/hello.so")
    message(FATAL_ERROR "deployed file missing: ${WORK_DIR}/dst/hello.so")
endif()
file(READ "${WORK_DIR}/dst/hello.so" _content)
if(NOT _content STREQUAL "build-one")
    message(FATAL_ERROR "deployed content wrong: '${_content}'")
endif()

# --- 2. Overwrite: a changed build replaces the artifact ---------------------
# (The rename-based atomicity itself -- old inode surviving for a running
# dlopen'd holder -- is a kernel property this script cannot assert; what is
# testable here is that a rebuild propagates and no temp file is left behind.)

file(WRITE "${WORK_DIR}/src/hello.so" "build-two")
deploy_run(0)
file(READ "${WORK_DIR}/dst/hello.so" _content)
if(NOT _content STREQUAL "build-two")
    message(FATAL_ERROR "redeploy did not overwrite: '${_content}'")
endif()

file(GLOB _leftover LIST_DIRECTORIES false "${WORK_DIR}/dst/*" "${WORK_DIR}/dst/.*")
list(FILTER _leftover EXCLUDE REGEX "/\\.\\.?$")
list(LENGTH _leftover _count)
if(NOT _count EQUAL 1)
    message(FATAL_ERROR
        "expected exactly hello.so after deploy, found ${_count}: ${_leftover}")
endif()

# --- 3. Missing source is a loud failure, not a silent no-op -----------------

file(REMOVE "${WORK_DIR}/src/hello.so")
deploy_run(1)

message(STATUS "deploy_user_plugin_test: all assertions passed")
