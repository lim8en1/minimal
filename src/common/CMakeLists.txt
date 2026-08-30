add_library(minimal_common STATIC
    abi_tag.cpp
    bundled_assets.cpp
    config_paths.cpp
    model_ref.cpp
    logger.cpp)
add_library(minimal::common ALIAS minimal_common)

target_include_directories(minimal_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# PUBLIC, not PRIVATE: the Logger type crosses into server_core's public headers
# (ToolContext) and from there into plugins via plugin_abi.h, so consumers need
# spdlog's headers too. The client already links it directly.
target_link_libraries(minimal_common PUBLIC spdlog::spdlog)

target_compile_definitions(minimal_common
    PUBLIC
        MINIMAL_ABI_TAG="${MINIMAL_ABI_TAG}"
        MINIMAL_VERSION="${PROJECT_VERSION}"
    # The plugin install location, PRIVATE because only bundled_assets.cpp
    # resolves it, and it must be the SAME value CMake installs to -- it used to
    # be hardcoded in the lookup, so overriding the cache variable moved the
    # install without moving the search. Plugins are the only bundled thing with
    # a runtime lookup at all: they are code the user does not edit, so they stay
    # in the install tree. Everything else is seeded into the config dir.
    PRIVATE
        MINIMAL_PLUGIN_DIR="${MINIMAL_PLUGIN_INSTALL_DIR}")

target_compile_options(minimal_common PRIVATE ${MINIMAL_WARNING_FLAGS})
