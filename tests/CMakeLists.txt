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
    agent_loop_test.cpp)

# minimal::test_support is the test-dependency bundle: it carries server_core,
# which carries common publicly. Listing those again here is redundant and makes
# CMake emit libminimal_common.a twice on the link line (harmless, but the linker
# warns about it). Depend on the bundle.
target_link_libraries(minimal_tests PRIVATE
    minimal::test_support
    Catch2::Catch2WithMain)

catch_discover_tests(minimal_tests)
