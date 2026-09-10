minimal_add_plugin(tools-python
    plugin.cpp
    python_tools.cpp
    python_interpreter.cpp
    python_process.cpp
    python_env.cpp
    python_settings.cpp
    python_store.cpp
    python_command.cpp)

# RE2: the linear-time guarantee python_output's model-authored `grep` pattern
# rests on (see tools-search/CMakeLists.txt for the same reasoning).
target_link_libraries(tools-python PRIVATE minimal_re2)
