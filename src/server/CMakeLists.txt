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
    sidecar/process.cpp
    sidecar/capped_buffer.cpp
    sidecar/drain.cpp
    sidecar/task_registry.cpp
    sidecar/lifetime_watch.cpp
    tools/permission.cpp
    tools/permission_advisor.cpp
    tools/path_confinement.cpp
    tools/pattern_match.cpp
    tools/file_access.cpp
    tools/atomic_write.cpp
    net/sse_parser.cpp
    net/http_client.cpp
    llm/token_accounting.cpp
    config/frontmatter.cpp
    config/permission_codec.cpp
    config/model_catalog_service.cpp
    config/providers_config.cpp
    config/settings_config.cpp
    config/permissions_config.cpp
    config/hooks_config.cpp
    config/parse_util.cpp
    config/config_bundle.cpp
    config/skill_registry.cpp
    config/project_dirs.cpp
    config/project_context.cpp
    startup/sanity_check.cpp
    hooks/hook_dispatcher.cpp
    hooks/hook_shell.cpp
    hooks/hook_observer_bridge.cpp
    agent/agent_preset_frontmatter.cpp
    agent/permission_resolution.cpp
    agent/agent_preset_registry.cpp
    agent/mention_resolver.cpp
    agent/mention_expansion.cpp
    plugins/name_resolution.cpp
    agent/compaction_boundary.cpp
    agent/transcript_repair.cpp
    agent/compaction_strategy.cpp
    agent/compaction_trigger.cpp
    agent/context_compactor.cpp
    agent/structured_compaction.cpp
    agent/agent_loop.cpp
    agent/compaction_controller.cpp
    agent/request_builder.cpp
    agent/retry_policy.cpp
    agent/tool_result_caps.cpp
    agent/tool_round.cpp
    agent/working_history.cpp
    agent/provider_instance_cache.cpp
    agent/provider_resolver.cpp
    commands/command_registry.cpp
    commands/command_dispatcher.cpp
    commands/project_command_source.cpp
    agent/subagent_spawn.cpp
    agent/subagent_manager.cpp
    agent/spawn_subagent_tool.cpp
    tools/tool_registry.cpp
    tools/aliased_tool.cpp
    tools/tool_executor.cpp
    tools/permission_model.cpp
    tools/tool_pool.cpp
    tools/skill_tool.cpp
    llm/provider_factory_registry.cpp
    llm/model_catalog.cpp
    llm/metadata_catalog.cpp
    llm/variant_params.cpp
    plugins/plugin_loader.cpp
    plugins/plugin_rpc.cpp
    plugins/plugin_storage.cpp
    plugins/plugin_discovery.cpp
    rpc/proto_convert.cpp
    rpc/pending_requests.cpp
    rpc/event_broadcaster.cpp
    rpc/command_executor.cpp
    rpc/auth_provider.cpp
    rpc/auth_interceptor.cpp
    rpc/wire_permission_checker.cpp
    rpc/wire_confirmation_broker.cpp
    rpc/wire_plugin_prompt_broker.cpp
    rpc/session_wiring.cpp
    rpc/session_service.cpp
    rpc/server.cpp
    rpc/server_housekeeping.cpp
    agent/session_observer_registry.cpp
    agent/context_contributor.cpp
    agent/session.cpp
    agent/session_launch.cpp
    agent/session_registry.cpp
    agent/session_support.cpp
    agent/session_naming.cpp
    agent/signal_router.cpp
    session/ulid.cpp
    session/message_codec.cpp
    session/session_database.cpp
    session/sqlite_history_store.cpp
    session/session_reader.cpp)
add_library(minimal::server_core ALIAS minimal_server_core)

target_include_directories(minimal_server_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/sidecar/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/llm/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/net/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/hooks/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/commands/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/session/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/startup/include")

target_link_libraries(minimal_server_core
    PUBLIC
        minimal::common
        minimal::proto
        nlohmann_json::nlohmann_json
        yaml-cpp::yaml-cpp
    PRIVATE
        spdlog::spdlog
        CURL::libcurl
        SQLite3::SQLite3)

set_target_properties(minimal_server_core PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}")

# Same treatment as minimal_client_core: the core's own @rpath dependency on
# libminimal_proto resolves against this dylib's rpaths, not the executable's.
# Its other dependencies -- curl, sqlite, spdlog -- are gathered into
# lib/minimal/runtime (MINIMAL_RUNTIME_DEP_INSTALL_DIR, top-level
# CMakeLists.txt), so the second hop reaches them from lib/ where this core
# installs.
if(APPLE)
    set_property(TARGET minimal_server_core PROPERTY INSTALL_RPATH
        "@loader_path;@loader_path/${MINIMAL_LIBDIR_TO_RUNTIME_DEP_RPATH}")
else()
    set_property(TARGET minimal_server_core PROPERTY INSTALL_RPATH
        "$ORIGIN;$ORIGIN/${MINIMAL_LIBDIR_TO_RUNTIME_DEP_RPATH}")
endif()

add_executable(minimal-server main.cpp)
target_link_libraries(minimal-server PRIVATE minimal::server_core)

# The server loads its bundled plugins from the plugin DIRECTORY at startup,
# not through any link dependency -- so without this edge nothing orders the
# plugin builds after the host, and a partial build could produce a
# just-built server next to stale plugins. The startup sanity check treats a
# failed bundled plugin as fatal, so that pairing aborts startup outright
# (the "abi mismatch ... rebuild the plugin" failure mode). The aggregate
# target lives in plugins/CMakeLists.txt.
add_dependencies(minimal-server bundled_plugins)

# Find libminimal_server_core next to us both in the build tree and once
# installed. Plugins get the same treatment in plugins/CMakeLists.txt. The
# second hop reaches the gathered dependencies in lib/minimal/runtime -- the
# executable loads them through the core (DT_RPATH on Linux covers a
# dependency's own dependencies; macOS consults the executable's full rpath
# list for every @rpath lookup).
if(APPLE)
    set_target_properties(minimal-server PROPERTIES INSTALL_RPATH
        "@loader_path/../${CMAKE_INSTALL_LIBDIR};@loader_path/../${CMAKE_INSTALL_LIBDIR}/${MINIMAL_LIBDIR_TO_RUNTIME_DEP_RPATH}")
else()
    set_target_properties(minimal-server PROPERTIES INSTALL_RPATH
        "$ORIGIN/../${CMAKE_INSTALL_LIBDIR};$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/${MINIMAL_LIBDIR_TO_RUNTIME_DEP_RPATH}")
endif()

install(TARGETS minimal-server minimal_server_core
    RUNTIME_DEPENDENCY_SET minimal_runtime_deps
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
minimal_strip_debug_info(minimal-server "${CMAKE_INSTALL_BINDIR}")
minimal_strip_debug_info(minimal_server_core "${CMAKE_INSTALL_LIBDIR}")

target_compile_options(minimal_server_core PRIVATE ${MINIMAL_WARNING_FLAGS})
target_compile_options(minimal-server PRIVATE ${MINIMAL_WARNING_FLAGS})
