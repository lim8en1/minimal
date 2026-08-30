add_library(minimal_common STATIC
    abi_tag.cpp
    config_paths.cpp
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
        MINIMAL_VERSION="${PROJECT_VERSION}")

target_compile_options(minimal_common PRIVATE ${MINIMAL_WARNING_FLAGS})
