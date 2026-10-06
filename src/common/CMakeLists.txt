add_library(minimal_common STATIC
    abi_tag.cpp
    atomic_write.cpp
    bundled_assets.cpp
    config_reader.cpp
    config_paths.cpp
    env.cpp
    fd_hygiene.cpp
    model_ref.cpp
    read_file.cpp
    serial_executor.cpp
    logger.cpp
    logging_setup.cpp
    # The collision/alias mechanism (one namespace per NameTable) and the
    # plugins.json / client-plugins.json format: shared by both plugin hosts,
    # which each own their namespaces and policy.
    plugins/name_table.cpp
    plugins/plugin_config.cpp
    plugins/plugin_config_store.cpp)
add_library(minimal::common ALIAS minimal_common)

target_include_directories(minimal_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# PUBLIC, not PRIVATE: the Logger type crosses into server_core's public headers
# (ToolContext) and from there into plugins via plugin_abi.h, so consumers need
# spdlog's headers too. The client already links it directly.
target_link_libraries(minimal_common PUBLIC spdlog::spdlog nlohmann_json::nlohmann_json)

target_compile_definitions(minimal_common
    PUBLIC
        MINIMAL_ABI_TAG="${MINIMAL_ABI_TAG}"
        MINIMAL_VERSION="${PROJECT_VERSION}${MINIMAL_VERSION_SUFFIX}"
    # The plugin install location, PRIVATE because only bundled_assets.cpp
    # resolves it. It is the SAME value CMake installs to, so overriding the
    # cache variable moves the install and the search together. Plugins are the only bundled thing with
    # a runtime lookup at all: they are code the user does not edit, so they stay
    # in the install tree. Everything else is seeded into the config dir.
    PRIVATE
        MINIMAL_PLUGIN_DIR="${MINIMAL_PLUGIN_INSTALL_DIR}"
        MINIMAL_CLIENT_PLUGIN_DIR="${MINIMAL_CLIENT_PLUGIN_INSTALL_DIR}")

target_compile_options(minimal_common PRIVATE ${MINIMAL_WARNING_FLAGS})
