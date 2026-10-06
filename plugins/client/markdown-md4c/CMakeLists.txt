minimal_add_client_plugin(minimal_markdown_md4c
    md4c_engine_plugin.cpp
    md4c_engine.cpp
    markdown_renderer.cpp
    streaming_markdown_view.cpp)
target_link_libraries(minimal_markdown_md4c PRIVATE md4c::md4c)
