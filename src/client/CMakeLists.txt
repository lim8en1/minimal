# Core client library. SHARED, not STATIC, for exactly the reason the server's
# core is (plugins/CMakeLists.txt -- the linkage history): a plugin's
# dlopen(RTLD_NOW) must resolve every symbol it uses against the host image, and
# an archive member is linked in only if the host references it. A shared core
# gives one copy in memory, one set of globals, and ordinary symbol resolution,
# which is what lets client plugins link minimal::client_core directly.
add_library(minimal_client_core SHARED
    cli_args.cpp
    session_client.cpp
    rpc_queue.cpp
    picker_model.cpp
    agent_picker_model.cpp
    state.cpp
    keybindings.cpp
    keybinding_dispatch.cpp
    key_event_parser.cpp
    status_format.cpp
    ui_config.cpp
    chat_model.cpp
    scrollback_refresh.cpp
    tui_bootstrap.cpp
    plugins/registries.cpp
    plugins/plugin_discovery.cpp
    plugins/plugin_loader.cpp
    plugins/core_registrations.cpp
    plugins/host_seams.cpp
    view/tab_bar.cpp
    view/chat_view.cpp
    view/prompt_editor.cpp
    view/status_bar.cpp
    view/tools_panel.cpp
    view/session_picker.cpp
    view/agent_picker.cpp
    view/picker_controller.cpp
    view/tab_ui.cpp
    view/prompt_modal.cpp
    view/modal_host.cpp
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
        ftxui::screen
        ftxui::dom
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

# The TUI client and the headless -p mode are one binary: `minimal` dispatches on
# argv. -p is the smallest real client and doubles as the end-to-end test harness.
add_executable(minimal app.cpp)
target_link_libraries(minimal PRIVATE minimal::client_core)

# Find libminimal_client_core next to the binary, in the build tree and once
# installed -- the same treatment minimal-server gets.
if(APPLE)
    set_target_properties(minimal PROPERTIES
        INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/src/client")
else()
    set_target_properties(minimal PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/src/client")
endif()

install(TARGETS minimal minimal_client_core
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")

target_compile_options(minimal_client_core PRIVATE ${MINIMAL_WARNING_FLAGS})
target_compile_options(minimal PRIVATE ${MINIMAL_WARNING_FLAGS})
