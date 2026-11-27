minimal_add_plugin(node
    plugin.cpp
    node_config.cpp
    schema_cache.cpp
    submit_result_tool.cpp
    input_gate.cpp)
target_link_libraries(node PRIVATE nlohmann_json_schema_validator::validator)
