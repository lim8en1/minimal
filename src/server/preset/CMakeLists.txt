minimal_server_module(preset
    DEPS llm policy config tools
    SOURCES
        agent_preset_frontmatter.cpp
        permission_resolution.cpp
        agent_preset_registry.cpp
)
