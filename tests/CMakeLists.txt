# tests/support is a real target, not test-local code: plugin authors link the same
# fakes. That matters when most functionality ships as plugins.
add_library(minimal_test_support INTERFACE)
add_library(minimal::test_support ALIAS minimal_test_support)
target_include_directories(minimal_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(minimal_test_support INTERFACE minimal::server_core)

add_executable(minimal_tests
    permission_test.cpp
    permission_chain_test.cpp
    path_confinement_test.cpp
    sse_parser_test.cpp
    frontmatter_test.cpp
    name_registry_test.cpp
    compaction_boundary_test.cpp
    compaction_trigger_test.cpp
    context_compactor_test.cpp
    bundled_assets_test.cpp
    chat_request_bench.cpp
    client_args_test.cpp
    commands_test.cpp
    "${CMAKE_SOURCE_DIR}/plugins/server/commands-core/core_commands.cpp"
    token_accounting_test.cpp
    config_paths_test.cpp
    tool_executor_test.cpp
    agent_loop_test.cpp
    http_client_test.cpp
    openai_request_test.cpp
    openai_response_test.cpp
    openai_stream_test.cpp
    openai_error_mapping_test.cpp
    plugin_loader_test.cpp
    aliased_tool_test.cpp
    provider_factory_registry_test.cpp
    permission_codec_test.cpp
    providers_config_test.cpp
    mcps_config_test.cpp
    sanity_check_test.cpp
    settings_config_test.cpp
    permissions_config_test.cpp
    agent_preset_frontmatter_test.cpp
    permission_resolution_test.cpp
    agent_preset_registry_test.cpp
    provider_instance_cache_test.cpp
    file_access_test.cpp
    atomic_write_test.cpp
    tools_fs_test.cpp
    tools_bash_test.cpp
    skill_registry_test.cpp
    skill_tool_test.cpp
    proto_convert_test.cpp
    pending_requests_test.cpp
    event_queue_test.cpp
    command_executor_test.cpp
    auth_test.cpp
    wire_permission_checker_test.cpp
    session_observer_test.cpp
    session_store_test.cpp
    session_test.cpp
    session_registry_test.cpp
    subagent_spawn_test.cpp
    subagent_test.cpp
    session_service_test.cpp
    subagent_e2e_test.cpp
    headless_e2e_test.cpp
    color_downsample_test.cpp
    theme_test.cpp
    streaming_markdown_view_test.cpp
    syntax_highlighter_test.cpp
    session_state_test.cpp
    app_state_test.cpp
    # provider-openai's pure translation logic, compiled directly into the test
    # binary rather than via the plugin .so -- these files have no ABI/plugin
    # dependency, only core headers, so this is real linkage, not dlopen.
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai/openai_request.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai/openai_response.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai/openai_stream_reassembler.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai/openai_error_mapping.cpp"
    # Same pattern for tools-fs: pure logic depending only on core headers, so
    # linked directly rather than exercised through dlopen.
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs/fs_tools.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs/fs_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs/file_editor.cpp"
    # Same pattern for tools-bash. The execution tests fork real processes, so
    # they need the actual spawn code, not a seam around it.
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/bash_tool.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/bash_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/bash_session_state.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/command_scanner.cpp")

target_include_directories(minimal_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs")

target_include_directories(minimal_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai")

target_include_directories(minimal_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash")

# minimal::test_support is the test-dependency bundle: it carries server_core,
# which carries common publicly. Listing those again here is redundant and makes
# CMake emit libminimal_common.a twice on the link line (harmless, but the linker
# warns about it). Depend on the bundle.
add_subdirectory(fixtures)
add_dependencies(minimal_tests
    fixture-good fixture-collision fixture-bad-abi fixture-minimal fixture-not-a-plugin)
target_compile_definitions(minimal_tests PRIVATE
    MINIMAL_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixtures"
    MINIMAL_BUNDLED_DIR="${CMAKE_SOURCE_DIR}/bundled")

# The headless client is exercised as a real client here, so the test binary
# links it -- the E2E tier is the whole point of building -p first.
#
# SQLite3 directly: server_core links it PRIVATE (session_database.hpp keeps
# <sqlite3.h> out of its interface), but the session-store tests build fixtures
# (a hand-crafted newer-version database) and assert on internals
# (integrity_check, journal_mode, sqlite_master) that SessionDatabase
# deliberately does not expose.
target_link_libraries(minimal_tests PRIVATE
    minimal::test_support
    minimal::client_core
    Catch2::Catch2WithMain
    SQLite3::SQLite3)

catch_discover_tests(minimal_tests)

target_compile_options(minimal_tests PRIVATE ${MINIMAL_WARNING_FLAGS})

# Config seeding is CMake, so its test is CMake. Reimplementing the copy in C++
# to test it would assert that two copies of the logic agree -- which is the
# class of bug this arrangement exists to avoid.
add_test(NAME seed_config
         COMMAND "${CMAKE_COMMAND}"
                 -DSEED_SCRIPT=${CMAKE_SOURCE_DIR}/cmake/SeedConfig.cmake
                 -DBUNDLED_DIR=${CMAKE_SOURCE_DIR}/bundled
                 -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/seed_config_work
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/seed_config_test.cmake")
