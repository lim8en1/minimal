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
    pattern_match_test.cpp
    path_confinement_test.cpp
    json_depth_test.cpp
    sse_parser_test.cpp
    frontmatter_test.cpp
    name_registry_test.cpp
    compaction_boundary_test.cpp
    transcript_repair_test.cpp
    compaction_strategy_test.cpp
    compaction_trigger_test.cpp
    context_compactor_test.cpp
    structured_compaction_test.cpp
    bundled_assets_test.cpp
    examples_parse_test.cpp
    chat_request_bench.cpp
    client_args_test.cpp
    client_plugins_test.cpp
    commands_test.cpp
    "${CMAKE_SOURCE_DIR}/plugins/server/commands-core/core_commands.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/commands-md/commands_md.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/commands-md/commands_md_parse.cpp"
    commands_md_test.cpp
    "${CMAKE_SOURCE_DIR}/plugins/server/signals-message/message_tool.cpp"
    token_accounting_test.cpp
    cancel_token_test.cpp
    sidecar_test.cpp
    config_paths_test.cpp
    file_lock_test.cpp
    logger_test.cpp
    sqlite_util_test.cpp
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
    channels_config_test.cpp
    signals_message_test.cpp
    mcp_naming_test.cpp
    mcp_tool_test.cpp
    mcp_connection_test.cpp
    mcp_client_manager_test.cpp
    mcp_signal_channel_test.cpp
    signal_router_test.cpp
    session_mcp_test.cpp
    mcp_e2e_test.cpp
    sanity_check_test.cpp
    fd_hygiene_test.cpp
    settings_config_test.cpp
    parse_util_test.cpp
    read_file_test.cpp
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
    tool_registry_test.cpp
    skill_registry_test.cpp
    project_dirs_test.cpp
    project_context_test.cpp
    skill_tool_test.cpp
    proto_convert_test.cpp
    pending_requests_test.cpp
    event_queue_test.cpp
    event_broadcaster_test.cpp
    event_translator_test.cpp
    command_executor_test.cpp
    auth_test.cpp
    wire_permission_checker_test.cpp
    wire_confirmation_broker_test.cpp
    wire_ask_broker_test.cpp
    session_wiring_test.cpp
    session_observer_test.cpp
    context_contributor_test.cpp
    session_store_test.cpp
    session_naming_test.cpp
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
    server_spawn_test.cpp
    plugin_storage_test.cpp
    plugin_rpc_test.cpp
    checklist_test.cpp
    # The memory plugin: store, search, tools, embedding backends, tagger and
    # context index are pure plugin logic over core headers; plugin.cpp (the
    # extern "C" entry points) stays out, like every other bundled plugin.
    # Same pattern as checklist above.
    memory_test.cpp
    "${CMAKE_SOURCE_DIR}/plugins/server/memory/memory_store.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/memory/memory_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/memory/embedding_http.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/memory/embedding_sidecar.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/memory/embedding_service.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/memory/search.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/memory/ingest.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/memory/tagger.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/memory/tools_read.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/memory/tools_write.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/memory/memory_command.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/memory/context_index.cpp"
    # The checklist-VIEW client plugin: model (header-only), poller and panel
    # view are pure plugin logic over the ClientApi/FocusProvider seams,
    # compiled in with stub seams; checklist_view_plugin.cpp (the extern "C"
    # entry points) stays out, like every other bundled plugin.
    "${CMAKE_SOURCE_DIR}/plugins/client/checklist-view/checklist_poller.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/checklist-view/checklist_panel_view.cpp"
    checklist_view_test.cpp
    color_downsample_test.cpp
    theme_test.cpp
    ui_config_test.cpp
    config_file_io_test.cpp
    streaming_markdown_view_test.cpp
    syntax_highlighter_test.cpp
    session_state_test.cpp
    app_state_test.cpp
    view_factory_test.cpp
    chat_view_scroll_test.cpp
    tab_ui_test.cpp
    tab_bar_test.cpp
    status_bar_test.cpp
    utf8_test.cpp
    prompt_wrap_test.cpp
    prompt_editor_test.cpp
    modal_host_test.cpp
    prompt_modal_test.cpp
    per_tab_prompt_editor_test.cpp
    keybindings_test.cpp
    keybinding_dispatch_test.cpp
    key_event_parser_test.cpp
    rpc_queue_test.cpp
    picker_model_test.cpp
    agent_picker_model_test.cpp
    subagent_picker_model_test.cpp
    keymap_help_model_test.cpp
    model_picker_model_test.cpp
    filterable_menu_test.cpp
    model_ref_test.cpp
    tui_bootstrap_test.cpp
    chat_model_test.cpp
    # The logo plugin's generator: pure std, no ABI, so compiled in directly
    # like the server plugins' logic (logo_plugin.cpp itself stays out).
    "${CMAKE_SOURCE_DIR}/plugins/client/logo/logo_generator.cpp"
    logo_generator_test.cpp
    # The cost-view segment's pure formatters: header-only, so only the
    # include dir is needed (the plugin TU itself stays out).
    cost_view_format_test.cpp
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
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-anthropic/anthropic_variant_params.cpp"
    anthropic_request_test.cpp
    anthropic_response_test.cpp
    anthropic_stream_test.cpp
    anthropic_error_mapping_test.cpp
    anthropic_variant_params_test.cpp
    # Same pattern for tools-fs: pure logic depending only on core headers, so
    # linked directly rather than exercised through dlopen.
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs/fs_tools.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs/fs_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs/file_editor.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs/line_diff.cpp"
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
    # rates_refresh.cpp is the startup auto-refresh: the gate is tested pure,
    # the teardown join against the same unroutable ports.
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/cost_model.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/cost_store.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/cost_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/in_flight.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/cost_observer.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/cost_command.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/rates_fetch.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/rates_refresh.cpp"
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
    # tool-ask: the tool is pure plugin logic over ToolContext's ask seam.
    "${CMAKE_SOURCE_DIR}/plugins/server/tool-ask/ask_tool.cpp"
    tool_ask_test.cpp
    # Same pattern for tools-web: pure logic plus a real loopback HTTP server
    # in the test itself (no fake to paper over the transport layer).
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/web_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/url_policy.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/html_text.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/fetch_tool.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/search_tool.cpp"
    tools_web_test.cpp
    # Same pattern for typesafe-advisor: settings + decision + the retry/budget
    # HTTP loop, all compiled in; the .so itself is only exercised by the E2E.
    "${CMAKE_SOURCE_DIR}/plugins/server/typesafe-advisor/settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/typesafe-advisor/decision.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/typesafe-advisor/typesafe_client.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/typesafe-advisor/advisor.cpp"
    typesafe_advisor_test.cpp
    # Same pattern for tools-python. The interpreter tests fork real python
    # processes, so they need the actual spawn code, not a seam around it.
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-python/python_tools.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-python/python_interpreter.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-python/python_process.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-python/python_env.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-python/python_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-python/python_store.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-python/python_command.cpp"
    tools_python_test.cpp
    # tools-windbg moved OUT of the bundled set into the user plugin repos
    # (see PLUGINS.md "User plugins"); its test travels with the plugin and
    # registers into ctest from there, still linking minimal::test_support.
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
    "${CMAKE_SOURCE_DIR}/plugins/server/signals-message"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-search"
    "${CMAKE_SOURCE_DIR}/plugins/server/tool-ask"
    "${CMAKE_SOURCE_DIR}/plugins/server/session-titler"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker"
    "${CMAKE_SOURCE_DIR}/plugins/server/memory"
    "${CMAKE_SOURCE_DIR}/plugins/client/cost-view"
    "${CMAKE_SOURCE_DIR}/plugins/client/checklist-view")

target_include_directories(minimal_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-python"
    "${CMAKE_SOURCE_DIR}/plugins/server/typesafe-advisor"
    "${CMAKE_SOURCE_DIR}/plugins/client/logo")

# minimal::test_support is the test-dependency bundle: it carries server_core,
# which carries common publicly. Listing those again here is redundant and makes
# CMake emit libminimal_common.a twice on the link line (harmless, but the linker
# warns about it). Depend on the bundle.
add_subdirectory(fixtures)
add_dependencies(minimal_tests
    fixture-good fixture-collision fixture-bad-abi fixture-minimal fixture-not-a-plugin
    fixture-throws fixture-advisor typesafe-advisor-fixture fixture-service fixture-service-b
    fixture-hook
    fixture-client fixture-client-exclusive fixture-client-partial-throw fixture-endpoint
    fixture-client-bad-abi fixture-user)
target_compile_definitions(minimal_tests PRIVATE
    MINIMAL_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixtures"
    MINIMAL_USER_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixtures-deployed"
    MINIMAL_BUNDLED_DIR="${CMAKE_SOURCE_DIR}/bundled"
    MINIMAL_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples"
    MINIMAL_LOGO_PLUGIN="$<TARGET_FILE:minimal_logo>"
    MINIMAL_PROTO_DIR="${CMAKE_SOURCE_DIR}/proto"
    # The captured live Jev responses (fixtures/typesafe-advisor/): the
    # response-shape contract the advisor parses, kept as data not literals.
    TYPESAFE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/typesafe-advisor")

# The headless client is exercised as a real client here, so the test binary
# links it -- the E2E tier is the whole point of building -p first.
#
# SQLite3 comes in transitively via minimal::common (it links it PUBLIC --
# sqlite_util lives there), so it must NOT be named here as well: that puts
# libsqlite3.a on the link line twice and ld notes the duplicate. The
# session-store tests still see <sqlite3.h> and can build fixtures
# (a hand-crafted newer-version database) and assert on internals
# (integrity_check, journal_mode, sqlite_master) that SessionDatabase
# deliberately does not expose.
target_sources(minimal_tests PRIVATE support/test_main.cpp)
target_link_libraries(minimal_tests PRIVATE
    minimal::test_support
    minimal::client_core
    Catch2::Catch2
    # tools-search sources are compiled directly into the test binary (see the
    # sources list above), so RE2 comes with them -- same linkage as the plugin.
    minimal_re2
    # memory sources are compiled directly into the test binary (see the
    # sources list above), same as every other bundled plugin's non-entry-
    # point sources; the sidecar backend spawns a plain python subprocess
    # (minimal::sidecar, already in server_core), so unlike the old onnx
    # backend there is no extra native dependency to keep plugin-only.
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

# The auto-spawn E2E test execs the REAL minimal-server binary (spawn -> serve
# -> idle reaper self-exit), so it needs its path and its build dependency.
add_dependencies(minimal_tests minimal-server)
target_compile_definitions(minimal_tests PRIVATE
    MINIMAL_SPAWN_TEST_SERVER="$<TARGET_FILE:minimal-server>")

# The tests load bundled plugins by path (the logo client plugin) and spawn a
# server whose startup loads the bundled server plugins by directory -- all
# runtime loads with no ordinary link dependency ordering them. Without this
# edge, a partial build (`--target minimal_tests`) pairs a fresh test binary
# with stale plugin .so files, and the ABI-tag guard fails the load: the logo
# tests dead-end on record.ok(), the spawn test on the server's fatal
# startup sanity check. The aggregate target is the freshness coupling.
add_dependencies(minimal_tests bundled_plugins)

# Scripted stdio SIGNAL-CHANNEL server, hand-rolled (no cpp-mcp: its server
# cannot push a notification over stdio -- see fake_signal_channel.cpp and the
# note in mcp_fake_server.cpp). Deterministic single-threaded emission.
add_executable(minimal_signal_channel_server support/fake_signal_channel.cpp)
target_compile_options(minimal_signal_channel_server PRIVATE ${MINIMAL_WARNING_FLAGS})
add_dependencies(minimal_tests minimal_signal_channel_server)
target_compile_definitions(minimal_tests PRIVATE
    MINIMAL_SIGNAL_CHANNEL_SERVER="$<TARGET_FILE:minimal_signal_channel_server>")

# The http-side tests use cpp-mcp's bundled (header-only) httplib as an
# in-process streamable-http server fixture. SYSTEM: third-party header.
target_include_directories(minimal_tests SYSTEM PRIVATE
    "${cpp_mcp_SOURCE_DIR}/common")

# TSAN_OPTIONS suppresses the uninstrumented system-libsqlite3 races the
# multi-process E2E tests trigger (see tests/tsan.supp); ignored by non-TSan
# builds, where nothing reads the variable.
catch_discover_tests(minimal_tests
    PROPERTIES ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/tsan.supp")

target_compile_options(minimal_tests PRIVATE ${MINIMAL_WARNING_FLAGS})

# Live MCP smoke test against a real MCP server (stdio + streamable-http).
# Opt-in: OFF by default so CI never builds or runs it; when ON, a missing
# server is a FAIL, not a skip -- it is a hard requirement where enabled.
option(MINIMAL_ENABLE_LIVE_MCP_TESTS
       "Build the live MCP smoke test (needs 'gateway' on PATH or MINIMAL_MCP_LIVE_SERVER)" OFF)
if(MINIMAL_ENABLE_LIVE_MCP_TESTS)
    add_executable(minimal_mcp_live_test mcp_live_test.cpp)
    target_sources(minimal_mcp_live_test PRIVATE support/test_main.cpp)
    target_link_libraries(minimal_mcp_live_test PRIVATE
        minimal::test_support
        Catch2::Catch2)
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

# Same arrangement for the user-plugin deploy step (cmake/DeployUserPlugin.cmake):
# its logic is CMake, so its test is CMake.
add_test(NAME deploy_user_plugin
         COMMAND "${CMAKE_COMMAND}"
                 -DDEPLOY_SCRIPT=${CMAKE_SOURCE_DIR}/cmake/DeployUserPlugin.cmake
                 -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/deploy_user_plugin_work
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/deploy_user_plugin_test.cmake")
