# Core client library. SHARED, not STATIC, for exactly the reason the server's
# core is (plugins/CMakeLists.txt -- the linkage history): a plugin's
# dlopen(RTLD_NOW) must resolve every symbol it uses against the host image, and
# an archive member is linked in only if the host references it. A shared core
# gives one copy in memory, one set of globals, and ordinary symbol resolution,
# which is what lets client plugins link minimal::client_core directly.
add_library(minimal_client_core SHARED
    cli_args.cpp
    command_line.cpp
    command_router.cpp
    server_spawn.cpp
    session_client.cpp
    rpc_queue.cpp
    picker_model.cpp
    model_picker_model.cpp
    agent_picker_model.cpp
    subagent_picker_model.cpp
    keymap_help_model.cpp
    state.cpp
    command_catalog.cpp
    keybindings.cpp
    keybinding_dispatch.cpp
    key_event_parser.cpp
    status_format.cpp
    ui_config.cpp
    config_file_io.cpp
    chat_model.cpp
    scrollback_refresh.cpp
    tui_bootstrap.cpp
    plugins/registries.cpp
    plugins/plugin_discovery.cpp
    plugins/plugin_loader.cpp
    plugins/core_registrations.cpp
    plugins/core_display_renderers.cpp
    plugins/host_seams.cpp
    plugins/observer_events.cpp
    view/tab_bar.cpp
    view/chat_scroll_region.cpp
    view/chat_view.cpp
    view/prompt_editor.cpp
    view/prompt_wrap.cpp
    view/status_bar.cpp
    view/tools_panel.cpp
    view/filterable_menu.cpp
    view/session_picker.cpp
    view/model_picker.cpp
    view/agent_picker.cpp
    view/subagent_picker.cpp
    view/theme_picker.cpp
    view/keymap_help.cpp
    view/commands_help.cpp
    view/picker_controller.cpp
    view/tab_ui.cpp
    view/frame_renderer.cpp
    view/prompt_modal.cpp
    view/modal_host.cpp
    view/modal_stack.cpp
    render/color_downsample.cpp
    render/theme.cpp
    render/syntax_highlighter.cpp
    render/markdown_renderer.cpp
    render/streaming_markdown_view.cpp)
add_library(minimal::client_core ALIAS minimal_client_core)

target_include_directories(minimal_client_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/include")

# The vendored grammar set for the syntax highlighter. Populates and compiles
# one static library per grammar; see the module header for why this is not
# the grammar repos' own CMake.
include("${CMAKE_SOURCE_DIR}/cmake/TreeSitterGrammars.cmake")

# tree-sitter core is normalized at the top level as minimal_tree_sitter; the
# grammar set below supplies the per-language parsers.
target_link_libraries(minimal_client_core
    PUBLIC
        minimal::common
        minimal::proto
        nlohmann_json::nlohmann_json
        # ftxui::component alone: it publicly links ftxui::dom and ftxui::screen,
        # so naming them here too would put those archives on the link line
        # twice (ld notes the duplicate).
        ftxui::component
        md4c::md4c
        minimal_tree_sitter
    PRIVATE
        tsgrammar_cpp
        tsgrammar_python
        tsgrammar_javascript
        tsgrammar_typescript
        tsgrammar_bash
        tsgrammar_json
        tsgrammar_yaml
        tsgrammar_rust
        tsgrammar_go
        spdlog::spdlog)

set_target_properties(minimal_client_core PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}")

# The core itself depends on @rpath/libminimal_proto.0.dylib, and a dylib's
# @rpath deps resolve with the DYLIB's rpaths, not the executable's. It lives
# in lib/ next to libminimal_proto, so its own directory suffices; the second
# hop reaches the gathered dependencies in lib/minimal/runtime
# (MINIMAL_RUNTIME_DEP_INSTALL_DIR).
if(APPLE)
    set_property(TARGET minimal_client_core PROPERTY INSTALL_RPATH
        "@loader_path;@loader_path/${MINIMAL_LIBDIR_TO_RUNTIME_DEP_RPATH}")
else()
    set_property(TARGET minimal_client_core PROPERTY INSTALL_RPATH
        "$ORIGIN;$ORIGIN/${MINIMAL_LIBDIR_TO_RUNTIME_DEP_RPATH}")
endif()

# The TUI client and the headless -p mode are one binary: `minimal` dispatches on
# argv. -p is the smallest real client and doubles as the end-to-end test harness.
add_executable(minimal app.cpp client_entry.cpp headless.cpp tui_app.cpp)
target_link_libraries(minimal PRIVATE minimal::client_core)

# Find libminimal_client_core next to the binary, in the build tree and once
# installed -- the same treatment minimal-server gets. The second hop reaches
# the gathered dependencies in lib/minimal/runtime
# (MINIMAL_RUNTIME_DEP_INSTALL_DIR).
if(APPLE)
    set_target_properties(minimal PROPERTIES
        INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR};@loader_path/../${CMAKE_INSTALL_LIBDIR}/${MINIMAL_LIBDIR_TO_RUNTIME_DEP_RPATH}"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/src/client")
else()
    set_target_properties(minimal PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR};$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/${MINIMAL_LIBDIR_TO_RUNTIME_DEP_RPATH}"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/src/client")
endif()

install(TARGETS minimal minimal_client_core
    RUNTIME_DEPENDENCY_SET minimal_runtime_deps
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
minimal_strip_debug_info(minimal "${CMAKE_INSTALL_BINDIR}")
minimal_strip_debug_info(minimal_client_core "${CMAKE_INSTALL_LIBDIR}")

target_compile_options(minimal_client_core PRIVATE ${MINIMAL_WARNING_FLAGS})
target_compile_options(minimal PRIVATE ${MINIMAL_WARNING_FLAGS})
