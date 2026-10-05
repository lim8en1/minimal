minimal_server_module(plugins
    DEPS llm net session policy auth config hooks tools preset commands agent
    SOURCES
        name_resolution.cpp
        plugin_loader.cpp
        plugin_rpc.cpp
        plugin_storage.cpp
        plugin_discovery.cpp
)
