# tests/support is a real target, not test-local code: plugin authors link the same
# fakes. That matters when most functionality ships as plugins.
add_library(minimal_test_support INTERFACE)
add_library(minimal::test_support ALIAS minimal_test_support)
target_include_directories(minimal_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(minimal_test_support INTERFACE minimal::server_core)

add_executable(minimal_tests
    permission_test.cpp
    permission_chain_test.cpp
    path_confinement_test.cpp
    sse_parser_test.cpp
    frontmatter_test.cpp
    name_registry_test.cpp
    compaction_boundary_test.cpp
    token_accounting_test.cpp
    config_paths_test.cpp
    tool_executor_test.cpp
    agent_loop_test.cpp
    http_client_test.cpp
    openai_request_test.cpp
    openai_response_test.cpp
    openai_stream_test.cpp
    openai_error_mapping_test.cpp
    plugin_loader_test.cpp
    provider_factory_registry_test.cpp
    permission_codec_test.cpp
    providers_config_test.cpp
    settings_config_test.cpp
    permissions_config_test.cpp
    agent_preset_frontmatter_test.cpp
    permission_resolution_test.cpp
    agent_preset_registry_test.cpp
    file_access_test.cpp
    atomic_write_test.cpp
    tools_fs_test.cpp
    # provider-openai's pure translation logic, compiled directly into the test
    # binary rather than via the plugin .so -- these files have no ABI/plugin
    # dependency, only core headers, so this is real linkage, not dlopen.
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai/openai_request.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai/openai_response.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai/openai_stream_reassembler.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai/openai_error_mapping.cpp"
    # Same pattern for tools-fs: pure logic depending only on core headers, so
    # linked directly rather than exercised through dlopen.
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs/fs_tools.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs/fs_settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs/file_editor.cpp")

target_include_directories(minimal_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/tools-fs")

target_include_directories(minimal_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/provider-openai")

# minimal::test_support is the test-dependency bundle: it carries server_core,
# which carries common publicly. Listing those again here is redundant and makes
# CMake emit libminimal_common.a twice on the link line (harmless, but the linker
# warns about it). Depend on the bundle.
add_subdirectory(fixtures)
add_dependencies(minimal_tests
    fixture-good fixture-bad-abi fixture-minimal fixture-not-a-plugin)
target_compile_definitions(minimal_tests PRIVATE
    MINIMAL_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixtures")

target_link_libraries(minimal_tests PRIVATE
    minimal::test_support
    Catch2::Catch2WithMain)

catch_discover_tests(minimal_tests)
