# Atomic deploy of a built user plugin into the runtime user-plugin dir.
#
# Invoked as the POST_BUILD step of every target built with
# minimal_add_user_plugin:
#
#   cmake -DSOURCE_FILE=<artifact> -DDEPLOY_DIR=<config>/plugins \
#         -P DeployUserPlugin.cmake
#
# COPY + RENAME, not copy in place: the destination may be mapped by a RUNNING
# server or client (plugins stay dlopen'd for the process lifetime, and the
# build-test-edit loop rebuilds while one is open). Overwriting in place would
# swap the bytes under the running image; a rename within the same directory
# is atomic -- the old inode survives for anyone still holding it open, and
# the next dlopen sees the new build.
#
# NEVER PRUNES: the user plugin dir is also the documented drop location for
# by-hand installs, so this script must not "helpfully" remove files it did
# not produce. Retiring a plugin means deleting its artifact by hand.

if(NOT DEFINED SOURCE_FILE OR NOT DEFINED DEPLOY_DIR)
    message(FATAL_ERROR
        "DeployUserPlugin.cmake: both SOURCE_FILE and DEPLOY_DIR are required")
endif()
if(NOT EXISTS "${SOURCE_FILE}")
    message(FATAL_ERROR "DeployUserPlugin.cmake: SOURCE_FILE does not exist: ${SOURCE_FILE}")
endif()

file(MAKE_DIRECTORY "${DEPLOY_DIR}")
cmake_path(GET SOURCE_FILE FILENAME _plugin_name)
set(_tmp "${DEPLOY_DIR}/.${_plugin_name}.deploy-tmp")
file(COPY_FILE "${SOURCE_FILE}" "${_tmp}")
file(RENAME "${_tmp}" "${DEPLOY_DIR}/${_plugin_name}")
message(STATUS "minimal: deployed ${_plugin_name} -> ${DEPLOY_DIR}")
