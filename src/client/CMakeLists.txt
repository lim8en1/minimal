add_library(minimal_client_core STATIC
    session_client.cpp)
add_library(minimal::client_core ALIAS minimal_client_core)

target_include_directories(minimal_client_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(minimal_client_core
    PUBLIC
        minimal::common
        minimal::proto
    PRIVATE
        ftxui::screen
        ftxui::dom
        ftxui::component
        spdlog::spdlog)

# The TUI client and the headless -p mode are one binary: `minimal` dispatches on
# argv. -p is the smallest real client and doubles as the end-to-end test harness.
add_executable(minimal app.cpp)
target_link_libraries(minimal PRIVATE minimal::client_core)
install(TARGETS minimal RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
