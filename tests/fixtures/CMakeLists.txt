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

# The CLIENT twin: links the SHARED client core (one copy in memory, ordinary
# symbol resolution -- the same reason the server fixtures link server core).
function(minimal_add_client_fixture_plugin name source)
    add_library(${name} MODULE ${source})
    target_link_libraries(${name} PRIVATE minimal::client_core)
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
# The decision-advisor E2E: the REAL DecisionAdvisor (the plugin's own sources)
# behind a fixture .so, so the session_service E2E loads shipped code through
# the real loader rather than a hand-rolled advisor.
minimal_add_fixture_plugin(decision-advisor-fixture fixture_decision_advisor.cpp)
target_sources(decision-advisor-fixture PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/decision-advisor/settings.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/decision-advisor/decision.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/decision-advisor/systemone_client.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/server/decision-advisor/advisor.cpp")
target_include_directories(decision-advisor-fixture PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/server/decision-advisor")
target_compile_definitions(decision-advisor-fixture PRIVATE
    DECISION_ADVISOR_BUNDLED_FILE="${CMAKE_SOURCE_DIR}/bundled/decision-advisor.json")
minimal_add_fixture_plugin(fixture-service    fixture_service.cpp)
minimal_add_fixture_plugin(fixture-service-b  fixture_service_b.cpp)
minimal_add_fixture_plugin(fixture-hook       fixture_hook.cpp)
minimal_add_fixture_plugin(fixture-lifecycle  fixture_lifecycle.cpp)
minimal_add_fixture_plugin(fixture-endpoint   fixture_endpoint.cpp)
minimal_add_fixture_plugin(fixture-project-commands fixture_project_commands.cpp)
minimal_add_fixture_plugin(fixture-config     fixture_config.cpp)

minimal_add_client_fixture_plugin(fixture-client           fixture_client_good.cpp)
minimal_add_client_fixture_plugin(fixture-client-exclusive fixture_client_exclusive.cpp)
minimal_add_client_fixture_plugin(fixture-client-partial-throw fixture_client_partial_throw.cpp)
minimal_add_client_fixture_plugin(fixture-client-bad-abi   fixture_client_bad_abi.cpp)

# The USER-plugin path: the same well-formed fixture source, built through
# minimal_add_user_plugin -- the function user plugin trees register with --
# with the deploy dir pointed INSIDE the build tree, so the suite never writes
# into a developer's real config dir. Proves the user function produces a
# loadable artifact from a config-dir-shaped location (plugin_loader_test
# loads the deployed copy), and that the POST_BUILD deploy step fires on a
# normal build.
set(MINIMAL_USER_PLUGIN_KIND server)
# CMAKE_CURRENT_BINARY_DIR here is <build>/tests/fixtures; the deployed copy
# is asserted (and loaded) at <build>/tests/fixtures-deployed by
# plugin_loader_test, so anchor to the top build dir.
minimal_add_user_plugin(fixture-user
    DEPLOY_DIR "${CMAKE_BINARY_DIR}/tests/fixtures-deployed"
    fixture_good.cpp)
unset(MINIMAL_USER_PLUGIN_KIND)
