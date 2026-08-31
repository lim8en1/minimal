minimal_add_plugin(tools-search
    plugin.cpp
    search_settings.cpp
    glob_matcher.cpp
    ignore_rules.cpp
    tree_walker.cpp
    glob_tool.cpp
    grep_tool.cpp)

# RE2: the linear-time guarantee the model-authored-pattern decision rests on
# (see plugin.cpp and ARCHITECTURE.md).
target_link_libraries(tools-search PRIVATE minimal_re2)
