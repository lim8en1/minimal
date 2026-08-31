add_library(minimal_client_core STATIC
    cli_args.cpp
    session_client.cpp
    state.cpp
    keybindings.cpp
    status_format.cpp
    chat_model.cpp
    view/tab_bar.cpp
    view/chat_view.cpp
    view/prompt_editor.cpp
    view/status_bar.cpp
    view/tools_panel.cpp
    view/session_picker.cpp
    view/prompt_modal.cpp
    render/color_downsample.cpp
    render/theme.cpp
    render/syntax_highlighter.cpp
    render/markdown_renderer.cpp
    render/streaming_markdown_view.cpp)
add_library(minimal::client_core ALIAS minimal_client_core)

target_include_directories(minimal_client_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# The vendored grammar set for the syntax highlighter. Populates and compiles
# one static library per grammar; see the module header for why this is not
# the grammar repos' own CMake.
include("${CMAKE_SOURCE_DIR}/cmake/TreeSitterGrammars.cmake")

# tree-sitter's vcpkg package has been named both ways; the root CMakeLists
# finds whichever this install provides and normalizes to minimal::tree_sitter.
target_link_libraries(minimal_client_core
    PUBLIC
        minimal::common
        minimal::proto
        nlohmann_json::nlohmann_json
        ftxui::screen
        ftxui::dom
        ftxui::component
        md4c::md4c
        minimal::tree_sitter
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

# The TUI client and the headless -p mode are one binary: `minimal` dispatches on
# argv. -p is the smallest real client and doubles as the end-to-end test harness.
add_executable(minimal app.cpp)
target_link_libraries(minimal PRIVATE minimal::client_core)
install(TARGETS minimal RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

target_compile_options(minimal_client_core PRIVATE ${MINIMAL_WARNING_FLAGS})
target_compile_options(minimal PRIVATE ${MINIMAL_WARNING_FLAGS})
