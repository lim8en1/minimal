add_library(minimal_common STATIC
    abi_tag.cpp
    config_paths.cpp)
add_library(minimal::common ALIAS minimal_common)

target_include_directories(minimal_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_definitions(minimal_common
    PUBLIC
        MINIMAL_ABI_TAG="${MINIMAL_ABI_TAG}"
        MINIMAL_VERSION="${PROJECT_VERSION}")
