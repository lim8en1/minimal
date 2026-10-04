minimal_server_module(tools
    DEPS llm session policy config hooks
    SOURCES
        permission_advisor.cpp
        path_confinement.cpp
        file_access.cpp
        tool_registry.cpp
        aliased_tool.cpp
        tool_executor.cpp
        permission_model.cpp
        tool_pool.cpp
        skill_tool.cpp
)
