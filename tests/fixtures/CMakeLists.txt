# Real .so fixtures. A loader cannot be honestly tested without loading
# something: ABI-tag rejection, symbol lookup, and dlopen failure modes are all
# invisible to a mock.
function(minimal_add_fixture_plugin name source)
    add_library(${name} MODULE ${source})
    target_link_libraries(${name} PRIVATE minimal::server_core)
    set_target_properties(${name} PROPERTIES
        PREFIX "" OUTPUT_NAME "${name}" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

minimal_add_fixture_plugin(fixture-good        fixture_good.cpp)
minimal_add_fixture_plugin(fixture-collision   fixture_collision.cpp)
minimal_add_fixture_plugin(fixture-bad-abi     fixture_bad_abi.cpp)
minimal_add_fixture_plugin(fixture-minimal     fixture_minimal.cpp)
minimal_add_fixture_plugin(fixture-not-a-plugin fixture_not_a_plugin.cpp)
minimal_add_fixture_plugin(fixture-throws    fixture_throws.cpp)
minimal_add_fixture_plugin(fixture-advisor    fixture_advisor.cpp)
minimal_add_fixture_plugin(fixture-service    fixture_service.cpp)
minimal_add_fixture_plugin(fixture-service-b  fixture_service_b.cpp)
minimal_add_fixture_plugin(fixture-hook       fixture_hook.cpp)
