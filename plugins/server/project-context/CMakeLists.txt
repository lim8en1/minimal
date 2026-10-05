minimal_add_plugin(project-context
    plugin.cpp
    project_context.cpp)
target_include_directories(project-context PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
