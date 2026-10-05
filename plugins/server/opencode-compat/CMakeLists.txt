minimal_add_plugin(opencode-compat
    plugin.cpp
    opencode_providers.cpp)
target_include_directories(opencode-compat PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
