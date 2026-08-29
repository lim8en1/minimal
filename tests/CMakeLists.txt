# tests/support is a real target, not test-local code: plugin authors link the same
# fakes. That matters when most functionality ships as plugins.
add_library(minimal_test_support INTERFACE)
add_library(minimal::test_support ALIAS minimal_test_support)
target_include_directories(minimal_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(minimal_test_support INTERFACE minimal::server_core)

add_executable(minimal_tests
    permission_test.cpp
    config_paths_test.cpp)

target_link_libraries(minimal_tests PRIVATE
    minimal::test_support
    minimal::common
    minimal::server_core
    Catch2::Catch2WithMain)

catch_discover_tests(minimal_tests)
