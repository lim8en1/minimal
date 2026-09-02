# tests/support is a real target, not test-local code: plugin authors link the same
# fakes. That matters when most functionality ships as plugins.
add_library(minimal_test_support INTERFACE)
add_library(minimal::test_support ALIAS minimal_test_support)
target_include_directories(minimal_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(minimal_test_support INTERFACE minimal::server_core)

add_executable(minimal_tests
    permission_test.cpp
    permission_advisor_test.cpp
    permission_chain_test.cpp
    path_confinement_test.cpp
    sse_parser_test.cpp
    frontmatter_test.cpp
    name_registry_test.cpp
    compaction_boundary_test.cpp
    compaction_strategy_test.cpp
    compaction_trigger_test.cpp
    context_compactor_test.cpp
    bundled_assets_test.cpp
    chat_request_bench.cpp
    client_args_test.cpp
    commands_test.cpp
    "${CMAKE_SOURCE_DIR}/plugins/server/commands-core/core_commands.cpp"
    token_accounting_test.cpp
    cancel_token_test.cpp
    config_paths_test.cpp
    logger_test.cpp
    tool_executor_test.cpp
    agent_loop_test.cpp
    http_client_test.cpp
    openai_request_test.cpp
    openai_response_test.cpp
    openai_stream_test.cpp
    openai_error_mapping_test.cpp
    plugin_loader_test.cpp
    plugin_discovery_test.cpp
    aliased_tool_test.cpp
    provider_factory_registry_test.cpp
    model_catalog_test.cpp
    model_catalog_live_test.cpp
    permission_codec_test.cpp
    providers_config_test.cpp
    mcps_config_test.cpp
    mcp_naming_test.cpp
    mcp_tool_test.cpp
    mcp_connection_test.cpp
    mcp_client_manager_test.cpp
    session_mcp_test.cpp
    mcp_e2e_test.cpp
    sanity_check_test.cpp
    fd_hygiene_test.cpp
    settings_config_test.cpp
    permissions_config_test.cpp
    agent_preset_frontmatter_test.cpp
    permission_resolution_test.cpp
    agent_preset_registry_test.cpp
    provider_instance_cache_test.cpp
    file_access_test.cpp
    atomic_write_test.cpp
    tools_fs_test.cpp
    tools_search_test.cpp
    tools_bash_test.cpp
    tool_pool_test.cpp
    skill_registry_test.cpp
    project_context_test.cpp
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
    rewind_test.cpp
    session_registry_test.cpp
    session_support_test.cpp
    subagent_spawn_test.cpp
    subagent_test.cpp
    tool_history_test.cpp
    session_service_test.cpp
    subagent_e2e_test.cpp
    headless_e2e_test.cpp
    plugin_storage_test.cpp
    plugin_rpc_test.cpp
    checklist_test.cpp
    color_downsample_test.cpp
    theme_test.cpp
    streaming_markdown_view_test.cpp
    syntax_highlighter_test.cpp
    session_state_test.cpp
    app_state_test.cpp
    view_factory_test.cpp
    modal_host_test.cpp
    keybindings_test.cpp
    keybinding_dispatch_test.cpp
    key_event_parser_test.cpp
    rpc_queue_test.cpp
    picker_model_test.cpp
    chat_model_test.cpp
    mention_resolver_test.cpp
    mention_expansion_test.cpp
    # provider-openai's pure translation logic, compiled directly into the test
    # binary rather than via the plugin .so -- these files have no ABI/plugin
    # dependency, only core headers, so this is real linkage, not dlopen.
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai/openai_request.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai/openai_response.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai/openai_stream_reassembler.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai/openai_error_mapping.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai/openai_model_catalog.cpp"
    # Same pattern for provider-anthropic.
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-anthropic/anthropic_request.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-anthropic/anthropic_response.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-anthropic/anthropic_stream_reassembler.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-anthropic/anthropic_error_mapping.cpp"
    anthropic_request_test.cpp
    anthropic_response_test.cpp
    anthropic_stream_test.cpp
    anthropic_error_mapping_test.cpp
    # Same pattern for tools-fs: pure logic depending only on core headers, so
    # linked directly rather than exercised through dlopen.
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs/fs_tools.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs/fs_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs/file_editor.cpp"
    # Same pattern for tools-bash. The execution tests fork real processes, so
    # they need the actual spawn code, not a seam around it.
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/bash_tool.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/bash_process.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/background_registry.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/bash_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/bash_session_state.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/command_scanner.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/restart_shell.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/tasks_command.cpp"
    # Same pattern for the titler: plugin logic over core headers, linked
    # directly so the SessionSupportService seam is tested against a fake.
    "${CMAKE_SOURCE_DIR}/plugins/server/session-titler/titler.cpp"
    titler_test.cpp
    bash_background_test.cpp
    # Same pattern for checklist: store, tool and rpc service are pure plugin
    # logic over core headers; plugin.cpp (the extern "C" entry points) stays
    # out, like every other bundled plugin.
    "${CMAKE_SOURCE_DIR}/plugins/server/checklist/checklist_store.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/checklist/checklist_tool.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/checklist/rpc_service.cpp"
    # Same pattern for cost-tracker. rates_fetch.cpp is the shared fetch path;
    # its network test uses an unroutable port, never the live snapshot.
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/cost_model.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/cost_store.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/cost_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/cost_observer.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/cost_command.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/update_rates_command.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/rates_fetch.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/cost_tracking_service_impl.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/rpc_service.cpp"
    # Same pattern for tools-search. The walker forks nothing and needs the
    # real filesystem only, so the whole plugin compiles in directly.
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-search/search_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-search/glob_matcher.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-search/ignore_rules.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-search/tree_walker.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-search/glob_tool.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-search/grep_tool.cpp"
    # Same pattern for tools-web: pure logic plus a real loopback HTTP server
    # in the test itself (no fake to paper over the transport layer).
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/web_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/url_policy.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/html_text.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/fetch_tool.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/search_tool.cpp"
    tools_web_test.cpp
    cost_tracker_test.cpp
    hooks_config_test.cpp
    hook_dispatcher_test.cpp
    hooks_tool_executor_test.cpp
    hooks_session_test.cpp
    service_registry_test.cpp)

target_include_directories(minimal_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs")

target_include_directories(minimal_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai")

target_include_directories(minimal_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-anthropic")

target_include_directories(minimal_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash")

target_include_directories(minimal_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/checklist"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-search"
    "${CMAKE_SOURCE_DIR}/plugins/server/session-titler"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker")

target_include_directories(minimal_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web")

# minimal::test_support is the test-dependency bundle: it carries server_core,
# which carries common publicly. Listing those again here is redundant and makes
# CMake emit libminimal_common.a twice on the link line (harmless, but the linker
# warns about it). Depend on the bundle.
add_subdirectory(fixtures)
add_dependencies(minimal_tests
    fixture-good fixture-collision fixture-bad-abi fixture-minimal fixture-not-a-plugin
    fixture-throws fixture-advisor fixture-service fixture-service-b fixture-hook
    fixture-endpoint)
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
    SQLite3::SQLite3
    # tools-search sources are compiled directly into the test binary (see the
    # sources list above), so RE2 comes with them -- same linkage as the plugin.
    minimal_re2
    # command_scanner.cpp is compiled directly into the test binary (see the
    # sources list above), so the scanner's tree-sitter dependencies come with
    # it -- same linkage as the plugin.
    minimal_tree_sitter
    minimal_tree_sitter_bash)

# Scripted stdio MCP server spawned by the MCP tests as a real child process.
# Island build like mcp_stdio_shim.cpp: cpp-mcp's bundled json only.
add_executable(minimal_mcp_fake_server support/mcp_fake_server.cpp)
target_include_directories(minimal_mcp_fake_server SYSTEM PRIVATE
    "${cpp_mcp_SOURCE_DIR}/include"
    "${cpp_mcp_SOURCE_DIR}/common")
target_link_libraries(minimal_mcp_fake_server PRIVATE mcp)
target_compile_options(minimal_mcp_fake_server PRIVATE ${MINIMAL_WARNING_FLAGS})
add_dependencies(minimal_tests minimal_mcp_fake_server)
target_compile_definitions(minimal_tests PRIVATE
    MINIMAL_MCP_FAKE_SERVER="$<TARGET_FILE:minimal_mcp_fake_server>")

# The http-side tests use cpp-mcp's bundled (header-only) httplib as an
# in-process streamable-http server fixture. SYSTEM: third-party header.
target_include_directories(minimal_tests SYSTEM PRIVATE
    "${cpp_mcp_SOURCE_DIR}/common")

catch_discover_tests(minimal_tests)

target_compile_options(minimal_tests PRIVATE ${MINIMAL_WARNING_FLAGS})

# Live MCP smoke test against a real MCP server (stdio + streamable-http).
# Opt-in: OFF by default so CI never builds or runs it; when ON, a missing
# server is a FAIL, not a skip -- it is a hard requirement where enabled.
option(MINIMAL_ENABLE_LIVE_MCP_TESTS
       "Build the live MCP smoke test (needs 'gateway' on PATH or MINIMAL_MCP_LIVE_SERVER)" OFF)
if(MINIMAL_ENABLE_LIVE_MCP_TESTS)
    add_executable(minimal_mcp_live_test mcp_live_test.cpp)
    target_link_libraries(minimal_mcp_live_test PRIVATE
        minimal::test_support
        Catch2::Catch2WithMain)
    target_compile_options(minimal_mcp_live_test PRIVATE ${MINIMAL_WARNING_FLAGS})
    catch_discover_tests(minimal_mcp_live_test)
endif()

# Config seeding is CMake, so its test is CMake. Reimplementing the copy in C++
# to test it would assert that two copies of the logic agree -- which is the
# class of bug this arrangement exists to avoid.
add_test(NAME seed_config
         COMMAND "${CMAKE_COMMAND}"
                 -DSEED_SCRIPT=${CMAKE_SOURCE_DIR}/cmake/SeedConfig.cmake
                 -DBUNDLED_DIR=${CMAKE_SOURCE_DIR}/bundled
                 -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/seed_config_work
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/seed_config_test.cmake")
