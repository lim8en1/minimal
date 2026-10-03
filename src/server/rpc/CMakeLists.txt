minimal_server_module(rpc
    DEPS llm session policy auth config tools preset commands agent plugins
    SOURCES
        proto_convert.cpp
        pending_requests.cpp
        event_broadcaster.cpp
        command_executor.cpp
        auth_interceptor.cpp
        wire_permission_checker.cpp
        wire_confirmation_broker.cpp
        wire_plugin_prompt_broker.cpp
        session_wiring.cpp
        stream_attachment.cpp
        session_service.cpp
        server.cpp
        server_housekeeping.cpp
)
