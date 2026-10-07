# tests/support is a real target, not test-local code: plugin authors link the same
# fakes. That matters when most functionality ships as plugins.
add_library(minimal_test_support INTERFACE)
add_library(minimal::test_support ALIAS minimal_test_support)
target_include_directories(minimal_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(minimal_test_support INTERFACE minimal::server_core)

# Test binaries are EXCLUDE_FROM_ALL: a plain build (`make build`) skips them.
# `minimal_test_binaries` (below) builds them; `make test` does so before ctest.
add_executable(minimal_tests EXCLUDE_FROM_ALL
    permission_test.cpp
    permission_advisor_test.cpp
    permission_chain_test.cpp
    pattern_match_test.cpp
    path_confinement_test.cpp
    json_depth_test.cpp
    sse_parser_test.cpp
    frontmatter_test.cpp
    name_table_test.cpp
    name_resolution_test.cpp
    compaction_boundary_test.cpp
    transcript_repair_test.cpp
    compaction_strategy_test.cpp
    compaction_trigger_test.cpp
    context_compactor_test.cpp
    structured_compaction_test.cpp
    bundled_assets_test.cpp
    examples_parse_test.cpp
    chat_request_bench.cpp
    permission_bench.cpp
    client_args_test.cpp
    client_plugins_test.cpp
    commands_test.cpp
    command_catalog_test.cpp
    command_router_test.cpp
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
    periodic_task_test.cpp
    sqlite_util_test.cpp
    tool_executor_test.cpp
    permission_model_test.cpp
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
    config_schema_test.cpp
    plugin_config_store_test.cpp
    model_catalog_live_test.cpp
    permission_codec_test.cpp
    providers_config_test.cpp
    opencode_compat_test.cpp
    secrets_test.cpp
    "${CMAKE_SOURCE_DIR}/plugins/server/skills/skill_registry.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/skills/skill_access.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/skills/skill_tool.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/skills/skills_model.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/skills/skills_preset.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/compaction-structured/structured_compaction.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/project-context/project_context.cpp"
    skills_plugin_test.cpp
    "${CMAKE_SOURCE_DIR}/plugins/server/opencode-compat/opencode_providers.cpp"
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
    config_reader_test.cpp
    config_docs_drift_test.cpp
    last_selection_test.cpp
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
    wire_plugin_prompt_broker_test.cpp
    session_wiring_test.cpp
    session_observer_test.cpp
    context_contributor_test.cpp
    session_store_test.cpp
    session_naming_test.cpp
    session_test.cpp
    tool_source_activation_test.cpp
    pause_gate_test.cpp
    wake_policy_test.cpp
    project_trust_test.cpp
    attributed_registry_test.cpp
    serial_executor_test.cpp
    server_housekeeping_test.cpp
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
    "${CMAKE_SOURCE_DIR}/plugins/server/memory/runtime_factory.cpp"
    # The checklist-VIEW client plugin: model (header-only), feed and panel
    # view are pure plugin logic over the ClientApi/FocusProvider seams,
    # compiled in with stub seams; checklist_view_plugin.cpp (the extern "C"
    # entry points) stays out, like every other bundled plugin.
    "${CMAKE_SOURCE_DIR}/plugins/client/checklist-view/checklist_feed.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/checklist-view/checklist_panel_view.cpp"
    checklist_view_test.cpp
    color_downsample_test.cpp
    theme_test.cpp
    ui_config_test.cpp
    config_file_io_test.cpp
    streaming_markdown_view_test.cpp
    syntax_highlighter_test.cpp
    common_utf8_test.cpp
    common_clock_test.cpp
    session_state_test.cpp
    session_state_invariants_test.cpp
    app_state_test.cpp
    observer_events_test.cpp
    view_factory_test.cpp
    chat_view_scroll_test.cpp
    tab_bar_test.cpp
    status_bar_test.cpp
    utf8_test.cpp
    prompt_wrap_test.cpp
    prompt_editor_test.cpp
    modal_host_test.cpp
    modal_stack_test.cpp
    ask_view_test.cpp
    "${CMAKE_SOURCE_DIR}/plugins/client/ask-view/ask_modal.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/ask-view/ask_display.cpp"
    per_tab_prompt_editor_test.cpp
    keybindings_test.cpp
    keybinding_dispatch_test.cpp
    key_event_parser_test.cpp
    rpc_queue_test.cpp
    picker_model_test.cpp
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
    # The cost-view segments: the pure formatters (header-only) and the
    # totals feed over stub ClientApi/FocusProvider seams (the plugin TU
    # itself, the extern "C" entry points, stays out).
    cost_view_format_test.cpp
    "${CMAKE_SOURCE_DIR}/plugins/client/cost-view/totals_feed.cpp"
    cost_view_feed_test.cpp
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
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-anthropic/anthropic_model_catalog.cpp"
    anthropic_model_catalog_test.cpp
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
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/stage_normalize.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/restart_shell.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-bash/tasks_command.cpp"
    # Same pattern for the titler: plugin logic over core headers, linked
    # directly so the SessionSupportService seam is tested against a fake.
    "${CMAKE_SOURCE_DIR}/plugins/server/session-titler/titler.cpp"
    titler_test.cpp
    bash_background_test.cpp
    # Same pattern for checklist: store, tool, hub, rpc service and endpoint
    # are pure plugin logic over core headers; plugin.cpp (the extern "C"
    # entry points) stays out, like every other bundled plugin.
    "${CMAKE_SOURCE_DIR}/plugins/server/checklist/checklist_store.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/checklist/checklist_tool.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/checklist/checklist_hub.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/checklist/rpc_service.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/checklist/endpoint.cpp"
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
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/session_index.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/totals_hub.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/cost-tracker/endpoint.cpp"
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
    "${CMAKE_SOURCE_DIR}/plugins/server/tool-ask/ask_types.cpp"
    tool_ask_test.cpp
    # Same pattern for tools-web: pure logic plus a real loopback HTTP server
    # in the test itself (no fake to paper over the transport layer).
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/web_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/url_policy.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/html_text.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/fetch_tool.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/search_tool.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-web/host_permission_model.cpp"
    tools_web_test.cpp
    tools_web_permission_test.cpp
    # Same pattern for decision-advisor: settings + decision + the retry/budget
    # HTTP loop, all compiled in; the .so itself is only exercised by the E2E.
    "${CMAKE_SOURCE_DIR}/plugins/server/decision-advisor/settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/decision-advisor/decision.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/decision-advisor/systemone_client.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/decision-advisor/advisor.cpp"
    decision_advisor_test.cpp
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
    service_registry_test.cpp
    # Same pattern for the mcp plugin: the whole client (manager, transports,
    # configs, permission model, command, endpoint, channel glue) is plugin
    # logic over core headers; plugin.cpp stays out.
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcp_client_manager.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcp_connection.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcp_tool.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcp_tool_source.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/env_interpolation.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/naming.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/signal_protocol.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcps_config.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/channels_config.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcp_permission_model.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcp_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcp_validation.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcp_channels.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcp_command.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcp_plugin_endpoint.cpp"
    mcp_plugin_test.cpp
    # The client half of the pair: the picker's pure row model.
    "${CMAKE_SOURCE_DIR}/plugins/client/mcp-view/mcp_picker_model.cpp"
    # config-view: its feed and component, over a fake endpoint.
    "${CMAKE_SOURCE_DIR}/plugins/client/config-view/config_view.cpp"
    schema_form_test.cpp
    # help-view: its row builders, views and commands, over a fake host.
    "${CMAKE_SOURCE_DIR}/plugins/client/help-view/help_view.cpp"
    help_view_test.cpp
    # theme-view: the picker and /theme, over a fake host.
    "${CMAKE_SOURCE_DIR}/plugins/client/theme-view/theme_view.cpp"
    theme_view_test.cpp
    # selection-view: the agent/model/subagent-model choosers, over a fake host.
    "${CMAKE_SOURCE_DIR}/plugins/client/selection-view/selection_view.cpp"
    selection_view_test.cpp
    # session-view: the session and subagent pickers, and their rows.
    "${CMAKE_SOURCE_DIR}/plugins/client/session-view/session_view.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/session-view/session_rows.cpp"
    session_view_test.cpp
    # session-status: the status bar's usual segments.
    "${CMAKE_SOURCE_DIR}/plugins/client/session-status/session_status.cpp"
    session_status_test.cpp
    # tab-bar: the row of tabs.
    "${CMAKE_SOURCE_DIR}/plugins/client/tab-bar/tab_bar.cpp"
    # tools-panel: every tab's tool calls.
    "${CMAKE_SOURCE_DIR}/plugins/client/tools-panel/tools_panel.cpp"
    tools_panel_test.cpp
    # prompt-editor: the prompt editor the editor tests drive.
    "${CMAKE_SOURCE_DIR}/plugins/client/prompt-editor/prompt_editor.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/prompt-editor/prompt_editor_factory.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/prompt-editor/prompt_wrap.cpp"
    # consent-prompts: the permission and confirmation prompt kinds.
    "${CMAKE_SOURCE_DIR}/plugins/client/consent-prompts/consent_modals.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/consent-prompts/consent_prompts.cpp"
    consent_prompts_test.cpp
    # ui-kit: the bundled widget kit, which every view test draws with.
    "${CMAKE_SOURCE_DIR}/plugins/client/ui-kit/ui_kit.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/ui-kit/modal_chrome.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/ui-kit/filterable_menu.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/ui-kit/schema_form.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/ui-kit/model_picker.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/ui-kit/model_picker_model.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/ui-kit/model_selector.cpp"
    # syntax-grammars: the bundled grammar set the highlighter tests use.
    "${CMAKE_SOURCE_DIR}/plugins/client/syntax-grammars/syntax_grammars.cpp"
    # markdown-md4c: the markdown engine the chat-view tests render with.
    "${CMAKE_SOURCE_DIR}/plugins/client/markdown-md4c/md4c_engine.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/markdown-md4c/markdown_renderer.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/client/markdown-md4c/streaming_markdown_view.cpp"
    task_worker_test.cpp
    model_selector_test.cpp
    mcp_view_test.cpp)

target_include_directories(minimal_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp/include"
    "${CMAKE_SOURCE_DIR}/plugins/server/mcp"
    "${CMAKE_SOURCE_DIR}/plugins/client/mcp-view"
    "${CMAKE_SOURCE_DIR}/plugins/client/config-view"
    "${CMAKE_SOURCE_DIR}/plugins/client/help-view"
    "${CMAKE_SOURCE_DIR}/plugins/client/theme-view"
    "${CMAKE_SOURCE_DIR}/plugins/client/selection-view"
    "${CMAKE_SOURCE_DIR}/plugins/client/session-view"
    "${CMAKE_SOURCE_DIR}/plugins/client/session-status"
    "${CMAKE_SOURCE_DIR}/plugins/client/ui-kit"
    "${CMAKE_SOURCE_DIR}/plugins/client/consent-prompts"
    "${CMAKE_SOURCE_DIR}/plugins/client/prompt-editor"
    "${CMAKE_SOURCE_DIR}/plugins/client/tools-panel"
    "${CMAKE_SOURCE_DIR}/plugins/client/tab-bar"
    "${CMAKE_SOURCE_DIR}/plugins/client/syntax-grammars"
    "${CMAKE_SOURCE_DIR}/plugins/client/markdown-md4c"
    "${CMAKE_SOURCE_DIR}/plugins/client/ask-view")

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
    "${CMAKE_SOURCE_DIR}/plugins/server/decision-advisor"
    "${CMAKE_SOURCE_DIR}/plugins/server/opencode-compat"
    "${CMAKE_SOURCE_DIR}/plugins/server/skills"
    "${CMAKE_SOURCE_DIR}/plugins/server/compaction-structured"
    "${CMAKE_SOURCE_DIR}/plugins/server/project-context"
    "${CMAKE_SOURCE_DIR}/plugins/client/logo")

# minimal::test_support is the test-dependency bundle: it carries server_core,
# which carries common publicly. Listing those again here is redundant and makes
# CMake emit libminimal_common.a twice on the link line (harmless, but the linker
# warns about it). Depend on the bundle.
add_subdirectory(fixtures EXCLUDE_FROM_ALL)   # built via minimal_tests, not `all`
add_dependencies(minimal_tests
    fixture-good fixture-collision fixture-bad-abi fixture-minimal fixture-not-a-plugin
    fixture-throws fixture-advisor decision-advisor-fixture fixture-service fixture-service-b
    fixture-hook
    fixture-lifecycle
    fixture-client fixture-client-exclusive fixture-client-partial-throw fixture-endpoint
    fixture-client-bad-abi fixture-user fixture-project-commands fixture-config)
target_compile_definitions(minimal_tests PRIVATE
    MINIMAL_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixtures"
    MINIMAL_USER_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixtures-deployed"
    MINIMAL_BUNDLED_DIR="${CMAKE_SOURCE_DIR}/bundled"
    MINIMAL_BUNDLED_SERVER_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins/server"
    MINIMAL_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples"
    MINIMAL_DOCS_DIR="${CMAKE_SOURCE_DIR}/docs"
    MINIMAL_LOGO_PLUGIN="$<TARGET_FILE:minimal_logo>"
    MINIMAL_CONFIG_VIEW_PLUGIN="$<TARGET_FILE:minimal_config_view>"
    MINIMAL_HELP_VIEW_PLUGIN="$<TARGET_FILE:minimal_help_view>"
    MINIMAL_THEME_VIEW_PLUGIN="$<TARGET_FILE:minimal_theme_view>"
    MINIMAL_SELECTION_VIEW_PLUGIN="$<TARGET_FILE:minimal_selection_view>"
    MINIMAL_SESSION_VIEW_PLUGIN="$<TARGET_FILE:minimal_session_view>"
    MINIMAL_SESSION_STATUS_PLUGIN="$<TARGET_FILE:minimal_session_status>"
    MINIMAL_UI_KIT_PLUGIN="$<TARGET_FILE:minimal_ui_kit>"
    MINIMAL_CONSENT_PROMPTS_PLUGIN="$<TARGET_FILE:minimal_consent_prompts>"
    MINIMAL_PROMPT_EDITOR_PLUGIN="$<TARGET_FILE:minimal_prompt_editor>"
    MINIMAL_PROTO_DIR="${CMAKE_SOURCE_DIR}/proto"
    # The captured live Jev responses (fixtures/decision-advisor/): the
    # response-shape contract the advisor parses, kept as data not literals.
    DECISION_ADVISOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/decision-advisor")

# The headless client is exercised as a real client here, so the test binary
# links it -- the E2E tier is the whole point of building -p first.
#
# SQLite3 comes in transitively via minimal::server_core (it links it PUBLIC --
# store's sqlite_util.hpp includes <sqlite3.h>), so it must NOT be named here as well: that puts
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

# Scripted stdio MCP server spawned by the MCP tests as a real child process
# (hand-rolled JSON-RPC, like the signal-channel fixture below).
add_executable(minimal_mcp_fake_server EXCLUDE_FROM_ALL support/mcp_fake_server.cpp)
target_link_libraries(minimal_mcp_fake_server PRIVATE nlohmann_json::nlohmann_json)
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

# Scripted stdio SIGNAL-CHANNEL server, hand-rolled. Deterministic
# single-threaded emission.
add_executable(minimal_signal_channel_server EXCLUDE_FROM_ALL support/fake_signal_channel.cpp)
target_compile_options(minimal_signal_channel_server PRIVATE ${MINIMAL_WARNING_FLAGS})
add_dependencies(minimal_tests minimal_signal_channel_server)
target_compile_definitions(minimal_tests PRIVATE
    MINIMAL_SIGNAL_CHANNEL_SERVER="$<TARGET_FILE:minimal_signal_channel_server>")

# The http-side tests use httplib as an in-process streamable-http server
# fixture (support/fake_http_mcp_server.hpp).
find_package(httplib CONFIG REQUIRED)
target_link_libraries(minimal_tests PRIVATE httplib::httplib)
# The bundled grammar set's parsers (syntax_grammars.cpp is compiled in).
target_link_libraries(minimal_tests PRIVATE
    tsgrammar_cpp tsgrammar_python tsgrammar_javascript tsgrammar_typescript tsgrammar_bash
    tsgrammar_json tsgrammar_yaml tsgrammar_rust tsgrammar_go
    md4c::md4c)

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
    add_executable(minimal_mcp_live_test EXCLUDE_FROM_ALL mcp_live_test.cpp)
    target_sources(minimal_mcp_live_test PRIVATE support/test_main.cpp
        "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcp_client_manager.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcp_connection.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcp_tool.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/server/mcp/env_interpolation.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/server/mcp/naming.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/server/mcp/signal_protocol.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/server/mcp/mcps_config.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/server/mcp/channels_config.cpp")
    target_include_directories(minimal_mcp_live_test PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/server/mcp/include")
    target_link_libraries(minimal_mcp_live_test PRIVATE
        minimal::test_support
        Catch2::Catch2)
    target_compile_options(minimal_mcp_live_test PRIVATE ${MINIMAL_WARNING_FLAGS})
    catch_discover_tests(minimal_mcp_live_test)
endif()

# The one target that builds every test binary. minimal_tests already depends
# on its fixtures (fake servers, minimal-server, bundled plugins).
add_custom_target(minimal_test_binaries)
add_dependencies(minimal_test_binaries minimal_tests)
if(MINIMAL_ENABLE_LIVE_MCP_TESTS)
    add_dependencies(minimal_test_binaries minimal_mcp_live_test)
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

# Layering of src/server/<module>: an include from a module that may not depend
# on its target fails here (scripts/check_module_deps.py has the layer table).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME module_deps
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_module_deps.py"
                     "${CMAKE_SOURCE_DIR}")
    # No hidden judgment calls (ARCHITECTURE.md "Policy constants"): every
    # number is a cited fact, a written derivation, or a policy.hpp entry.
    # Needs clang-tidy (LLVM); skipped where it is not installed.
    find_program(MINIMAL_CLANG_TIDY NAMES clang-tidy
                 HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
    if(MINIMAL_CLANG_TIDY)
        add_test(NAME policy_constants
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_policy_constants.py"
                         --build-dir "${CMAKE_BINARY_DIR}")
    endif()
    # Core + one provider is a working harness, and every bundled plugin can be
    # disabled alone (tests/plugin_matrix_test.py, against the real binaries).
    add_test(NAME plugin_matrix
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/plugin_matrix_test.py"
                     $<TARGET_FILE:minimal-server> $<TARGET_FILE:minimal>
                     "${CMAKE_BINARY_DIR}/plugins/server"
                     "${CMAKE_SOURCE_DIR}/cmake/SeedConfig.cmake" "${CMAKE_SOURCE_DIR}/bundled")
    set_tests_properties(plugin_matrix PROPERTIES TIMEOUT 600)
    # Plugins include only the SDK (scripts/sdk_headers.txt) or their own headers.
    add_test(NAME plugin_conformance
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_module_deps.py"
                     --plugins "${CMAKE_SOURCE_DIR}")
endif()
