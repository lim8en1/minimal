minimal_server_module(commands
    DEPS config tools preset
    SOURCES
        command_registry.cpp
        command_dispatcher.cpp
        project_command_source.cpp
)
