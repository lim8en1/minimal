add_library(minimal_common STATIC
    abi_tag.cpp
    bundled_assets.cpp
    config_paths.cpp
    fd_hygiene.cpp
    file_lock.cpp
    model_ref.cpp
    read_file.cpp
    serial_executor.cpp
    sqlite_util.cpp
    logger.cpp
    logging_setup.cpp
    # The NameRegistry moved here from src/server/plugins when the client host
    # (track E5) needed the same collision/alias rule: one rule, two loaders,
    # so the machinery cannot live on either side. name_resolution.cpp stayed
    # behind -- it feeds the registry from SERVER registry attributions.
    plugins/name_registry.cpp
    # The plugins.json / client-plugins.json format and plugin-file discovery,
    # shared by both hosts (they were drifting structural twins).
    plugins/plugin_config.cpp)
add_library(minimal::common ALIAS minimal_common)

target_include_directories(minimal_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# PUBLIC, not PRIVATE: the Logger type crosses into server_core's public headers
# (ToolContext) and from there into plugins via plugin_abi.h, so consumers need
# spdlog's headers too. The client already links it directly.
target_link_libraries(minimal_common PUBLIC spdlog::spdlog SQLite3::SQLite3 nlohmann_json::nlohmann_json)

target_compile_definitions(minimal_common
    PUBLIC
        MINIMAL_ABI_TAG="${MINIMAL_ABI_TAG}"
        MINIMAL_VERSION="${PROJECT_VERSION}${MINIMAL_VERSION_SUFFIX}"
    # The plugin install location, PRIVATE because only bundled_assets.cpp
    # resolves it, and it must be the SAME value CMake installs to -- it used to
    # be hardcoded in the lookup, so overriding the cache variable moved the
    # install without moving the search. Plugins are the only bundled thing with
    # a runtime lookup at all: they are code the user does not edit, so they stay
    # in the install tree. Everything else is seeded into the config dir.
    PRIVATE
        MINIMAL_PLUGIN_DIR="${MINIMAL_PLUGIN_INSTALL_DIR}"
        MINIMAL_CLIENT_PLUGIN_DIR="${MINIMAL_CLIENT_PLUGIN_INSTALL_DIR}")

target_compile_options(minimal_common PRIVATE ${MINIMAL_WARNING_FLAGS})
