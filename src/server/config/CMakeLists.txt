minimal_server_module(config
    DEPS llm policy
    SOURCES
        frontmatter.cpp
        permission_codec.cpp
        model_catalog_service.cpp
        providers_config.cpp
        provider_config_extensions.cpp
        settings_config.cpp
        permissions_config.cpp
        hooks_config.cpp
        parse_util.cpp
        config_bundle.cpp
        project_dirs.cpp
        project_trust.cpp
        project_context.cpp
)
