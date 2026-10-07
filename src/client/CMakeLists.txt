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
    plugin_command.cpp
    session_client.cpp
    rpc_queue.cpp
    keymap_help_model.cpp
    state.cpp
    session_components.cpp
    command_catalog.cpp
    keybindings.cpp
    keybinding_dispatch.cpp
    key_event_parser.cpp
    status_layout.cpp
    ui_config.cpp
    config_file_io.cpp
    last_selection.cpp
    chat_model.cpp
    scrollback_refresh.cpp
    tui_bootstrap.cpp
    plugins/registries.cpp
    plugins/plugin_discovery.cpp
    plugins/plugin_loader.cpp
    plugins/plain_markdown_engine.cpp
    plugins/host_seams.cpp
    plugins/observer_events.cpp
    plugins/task_worker.cpp
    view/prompt_host.cpp
    view/status_bar.cpp
    view/picker_controller.cpp
    view/tab_ui.cpp
    view/tab_activity.cpp
    view/chat_frame.cpp
    view/frame_renderer.cpp
    view/modal_host.cpp
    view/schema_form_model.cpp
    view/modal_stack.cpp
    render/color_downsample.cpp
    render/theme.cpp
    render/syntax_highlighter.cpp)
add_library(minimal::client_core ALIAS minimal_client_core)

target_include_directories(minimal_client_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/include")

# The vendored grammar set (the bundled syntax-grammars plugin links it).
# Populates and compiles one static library per grammar; see the module
# header for why this is not the grammar repos' own CMake.
include("${CMAKE_SOURCE_DIR}/cmake/TreeSitterGrammars.cmake")

# tree-sitter core is normalized at the top level as minimal_tree_sitter;
# the per-language parsers are the grammar plugins'.
target_link_libraries(minimal_client_core
    PUBLIC
        minimal::common
        minimal::proto
        nlohmann_json::nlohmann_json
        # ftxui's headers only: its code is linked into core whole, below.
        $<COMPILE_ONLY:ftxui::component>
        minimal_tree_sitter
    PRIVATE
        # ftxui is a static archive. Linked normally (and PUBLIC), every
        # plugin module re-linked the archive and pulled in its own copy of
        # each object core happened not to reference (CatchEvent, Checkbox,
        # ...): four plugins carried several hundred duplicate ftxui symbols
        # each. Whole, in core only, every ftxui symbol resolves against the
        # one shared copy -- the reason core is SHARED in the first place.
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,ftxui::component,ftxui::dom,ftxui::screen>"
        spdlog::spdlog)

# ftxui's exported targets chain component -> dom -> screen as plain link
# items, so dom and screen also reach the link line transitively with no
# feature. CMake refuses an item seen both with and without WHOLE_ARCHIVE;
# the override makes every occurrence whole.
set_target_properties(minimal_client_core PROPERTIES
    LINK_LIBRARY_OVERRIDE_ftxui::dom WHOLE_ARCHIVE
    LINK_LIBRARY_OVERRIDE_ftxui::screen WHOLE_ARCHIVE
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
add_executable(minimal app.cpp client_entry.cpp headless.cpp status_bar_command.cpp tui_app.cpp
               tui_commands.cpp)
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
