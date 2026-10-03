minimal_server_module(hooks
    DEPS llm config sidecar
    SOURCES
        hook_dispatcher.cpp
        hook_shell.cpp
)
