minimal_add_plugin(mcp
    plugin.cpp
    mcp_client_manager.cpp
    mcp_connection.cpp
    mcp_tool.cpp
    mcp_tool_source.cpp
    env_interpolation.cpp
    naming.cpp
    signal_protocol.cpp
    mcps_config.cpp
    channels_config.cpp
    mcp_permission_model.cpp
    mcp_settings.cpp
    mcp_validation.cpp
    mcp_channels.cpp
    mcp_command.cpp
    mcp_plugin_endpoint.cpp)

target_include_directories(mcp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}")
