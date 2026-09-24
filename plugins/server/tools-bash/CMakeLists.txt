minimal_add_plugin(tools-bash
    plugin.cpp
    bash_tool.cpp
    bash_process.cpp
    background_registry.cpp
    bash_settings.cpp
    bash_session_state.cpp
    command_scanner.cpp
    stage_normalize.cpp
    restart_shell.cpp
    tasks_command.cpp)

# tree-sitter core + the vendored bash grammar, for the command scanner that
# backs the permission disclosure (command_scanner.hpp).
target_link_libraries(tools-bash PRIVATE
    minimal_tree_sitter
    minimal_tree_sitter_bash)
