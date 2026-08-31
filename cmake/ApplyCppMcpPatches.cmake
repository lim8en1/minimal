# Applies the vendored cpp-mcp patches, idempotently.
#
# cpp-mcp is pinned (no releases), fetched by FetchContent, and carries two
# vendored patches:
#   - patches/cpp-mcp-child-pid.patch: an atomic process_id_ plus a
#     child_pid() accessor, so the harness can kill a wedged stdio child on
#     teardown -- see McpConnection::Teardown.
#   - patches/cpp-mcp-read-thread-eof.patch: the read thread fails every
#     pending request when the pipe closes, so a child that dies mid-request
#     unblocks its caller immediately instead of wedging it for the internal
#     60 s future timeout.
#
# Runs AFTER FetchContent_MakeAvailable, so ${cpp_mcp_SOURCE_DIR} is defined.
# Idempotent because `git apply --check` fails on an already-patched tree
# (existing build dirs) -- in that case the patch must already be applied,
# which is verified by a marker ("minimal-mcp patch") in the touched sources.
# Any other failure is fatal: a half-patched dependency must not build
# silently.
#
# git apply, not `patch`: the fetched source is a git clone, so the exact
# format-patch semantics (index lines, context matching) hold.

set(CPP_MCP_PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}/../patches")

find_package(Git REQUIRED)

file(GLOB cpp_mcp_patches "${CPP_MCP_PATCH_DIR}/*.patch")
foreach(patch IN LISTS cpp_mcp_patches)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check "${patch}"
        WORKING_DIRECTORY "${cpp_mcp_SOURCE_DIR}"
        RESULT_VARIABLE check_result
        OUTPUT_QUIET
        ERROR_QUIET)
    if(check_result EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
            WORKING_DIRECTORY "${cpp_mcp_SOURCE_DIR}"
            RESULT_VARIABLE apply_result)
        if(NOT apply_result EQUAL 0)
            message(FATAL_ERROR "cpp-mcp patch failed to apply: ${patch}")
        endif()
        message(STATUS "cpp-mcp: applied ${patch}")
    else()
        # Already applied (or the pinned SHA changed under us). Each patch's
        # marker proves it: if no marker is present, the tree diverged and the
        # patch needs a rebase -- fail loudly, never build silently.
        file(STRINGS "${cpp_mcp_SOURCE_DIR}/include/mcp_stdio_client.h" marker_header
             REGEX "minimal-mcp patch")
        file(STRINGS "${cpp_mcp_SOURCE_DIR}/src/mcp_stdio_client.cpp" marker_source
             REGEX "minimal-mcp patch")
        if(marker_header STREQUAL "" AND marker_source STREQUAL "")
            message(FATAL_ERROR
                    "cpp-mcp patch does not apply and its marker is missing: ${patch}. "
                    "Delete the build/_deps/cpp_mcp-src directory to re-fetch, "
                    "or rebase the patch onto the new pinned SHA.")
        endif()
        message(STATUS "cpp-mcp: already patched (${patch})")
    endif()
endforeach()
