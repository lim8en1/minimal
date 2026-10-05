minimal_add_plugin(skills
    plugin.cpp
    skill_registry.cpp
    skill_access.cpp
    skill_tool.cpp
    skills_model.cpp
    skills_preset.cpp)
target_include_directories(skills PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
