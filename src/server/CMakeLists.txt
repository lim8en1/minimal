# Core server library. Modules are folded into one target for now; they are split
# by directory rather than by CMake target because their headers are already
# namespaced and the dependency direction is enforced by include paths, not linkage.
add_library(minimal_server_core STATIC
    tools/permission.cpp
    tools/path_confinement.cpp
    llm/sse_parser.cpp
    llm/http_client.cpp
    llm/token_accounting.cpp
    config/frontmatter.cpp
    plugins/name_registry.cpp
    agent/compaction_boundary.cpp
    agent/agent_loop.cpp
    tools/tool_registry.cpp
    tools/tool_executor.cpp)
add_library(minimal::server_core ALIAS minimal_server_core)

target_include_directories(minimal_server_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/llm/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent/include")

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

# Where to look for bundled plugins, relative to the installed executable.
target_compile_definitions(minimal_server_core
    PRIVATE MINIMAL_PLUGIN_DIR="${MINIMAL_PLUGIN_INSTALL_DIR}")

add_executable(minimal-server main.cpp)
target_link_libraries(minimal-server PRIVATE minimal::server_core)

# Plugins resolve host symbols at dlopen time rather than linking the core static
# library (which would give each plugin its own copy of every global). That only
# works if the host exports its symbols: ENABLE_EXPORTS adds -rdynamic on ELF and
# is a no-op-but-correct on Mach-O. See src/plugins/CMakeLists.txt.
set_target_properties(minimal-server PROPERTIES ENABLE_EXPORTS ON)

install(TARGETS minimal-server RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
