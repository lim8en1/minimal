# Core server library. Modules are folded into one target for now; they are split
# by directory rather than by CMake target because their headers are already
# namespaced and the dependency direction is enforced by include paths, not linkage.
#
# SHARED, not STATIC, and that matters for plugins. An archive member is linked
# into an executable only if something references it, so with a static core a
# plugin calling e.g. ToolRegistry::Register would find that symbol absent from
# the host image and fail at dlopen(RTLD_NOW). A shared library gives one copy in
# memory, one set of globals, and ordinary symbol resolution -- which is what
# lets plugins link core directly instead of relying on export tricks.
add_library(minimal_server_core SHARED
    tools/permission.cpp
    tools/permission_advisor.cpp
    tools/path_confinement.cpp
    tools/file_access.cpp
    tools/atomic_write.cpp
    llm/sse_parser.cpp
    llm/http_client.cpp
    llm/token_accounting.cpp
    config/frontmatter.cpp
    config/permission_codec.cpp
    config/providers_config.cpp
    config/settings_config.cpp
    config/permissions_config.cpp
    config/hooks_config.cpp
    config/mcps_config.cpp
    config/skill_registry.cpp
    config/project_context.cpp
    config/sanity_check.cpp
    hooks/hook_dispatcher.cpp
    hooks/hook_shell.cpp
    hooks/hook_observer_bridge.cpp
    agent/agent_preset_frontmatter.cpp
    agent/permission_resolution.cpp
    agent/agent_preset_registry.cpp
    plugins/name_registry.cpp
    plugins/name_resolution.cpp
    agent/compaction_boundary.cpp
    agent/compaction_trigger.cpp
    agent/context_compactor.cpp
    agent/agent_loop.cpp
    agent/provider_instance_cache.cpp
    commands/command_registry.cpp
    commands/command_dispatcher.cpp
    agent/subagent_spawn.cpp
    agent/subagent_manager.cpp
    agent/spawn_subagent_tool.cpp
    tools/tool_registry.cpp
    tools/aliased_tool.cpp
    tools/tool_executor.cpp
    tools/tool_pool.cpp
    tools/skill_tool.cpp
    mcp/naming.cpp
    mcp/env_interpolation.cpp
    mcp/mcp_tool.cpp
    mcp/mcp_connection.cpp
    mcp/mcp_client_manager.cpp
    llm/provider_factory_registry.cpp
    llm/model_catalog.cpp
    plugins/plugin_loader.cpp
    plugins/plugin_rpc.cpp
    plugins/plugin_storage.cpp
    plugins/plugin_discovery.cpp
    rpc/proto_convert.cpp
    rpc/pending_requests.cpp
    rpc/command_executor.cpp
    rpc/auth_provider.cpp
    rpc/auth_interceptor.cpp
    rpc/wire_permission_checker.cpp
    rpc/wire_confirmation_broker.cpp
    rpc/session_service.cpp
    rpc/server.cpp
    agent/session_observer_registry.cpp
    agent/session.cpp
    agent/session_registry.cpp
    agent/session_support.cpp
    session/ulid.cpp
    session/message_codec.cpp
    session/session_database.cpp
    session/sqlite_history_store.cpp
    session/session_reader.cpp)
add_library(minimal::server_core ALIAS minimal_server_core)

target_include_directories(minimal_server_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/llm/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/hooks/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/commands/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/session/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/mcp/include")

# cpp-mcp vendors its own older nlohmann/json whose ABI namespace differs from
# vcpkg's, so the two cannot share a translation unit. The shim is compiled as
# an ISLAND: a static library whose one TU sees cpp-mcp's headers and no vcpkg
# json, exposing a string/bool facade (mcp_stdio_shim.hpp). SYSTEM on the
# include paths keeps third-party header warnings out of the zero-warnings gate.
add_library(minimal_mcp_shim STATIC mcp/mcp_stdio_shim.cpp)
target_include_directories(minimal_mcp_shim SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/mcp/include"
    "${cpp_mcp_SOURCE_DIR}/include"
    "${cpp_mcp_SOURCE_DIR}/common")
target_link_libraries(minimal_mcp_shim PUBLIC mcp)
target_compile_options(minimal_mcp_shim PRIVATE ${MINIMAL_WARNING_FLAGS})

target_link_libraries(minimal_server_core
    PUBLIC
        minimal::common
        minimal::proto
        nlohmann_json::nlohmann_json
        yaml-cpp::yaml-cpp
    PRIVATE
        spdlog::spdlog
        CURL::libcurl
        SQLite3::SQLite3
        minimal_mcp_shim)

set_target_properties(minimal_server_core PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}")

add_executable(minimal-server main.cpp)
target_link_libraries(minimal-server PRIVATE minimal::server_core)

# Find libminimal_server_core next to us both in the build tree and once
# installed. Plugins get the same treatment in plugins/CMakeLists.txt.
if(APPLE)
    set_target_properties(minimal-server PROPERTIES INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
else()
    set_target_properties(minimal-server PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

install(TARGETS minimal-server minimal_server_core
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")

target_compile_options(minimal_server_core PRIVATE ${MINIMAL_WARNING_FLAGS})
target_compile_options(minimal-server PRIVATE ${MINIMAL_WARNING_FLAGS})
