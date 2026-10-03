minimal_server_module(startup
    DEPS llm session policy config hooks tools preset commands agent plugins
    SOURCES
        sanity_check.cpp
)
