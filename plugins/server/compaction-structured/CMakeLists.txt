minimal_add_plugin(compaction-structured
    plugin.cpp
    structured_compaction.cpp)
target_include_directories(compaction-structured PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
