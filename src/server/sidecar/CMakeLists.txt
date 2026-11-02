minimal_server_module(sidecar
    DEPS llm policy
    SOURCES
        process.cpp
        capped_buffer.cpp
        drain.cpp
        memory_scope.cpp
        task_registry.cpp
)
