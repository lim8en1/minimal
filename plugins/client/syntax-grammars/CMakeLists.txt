minimal_add_client_plugin(minimal_syntax_grammars
    syntax_grammars_plugin.cpp
    syntax_grammars.cpp)
# The grammars themselves (cmake/TreeSitterGrammars.cmake): static, so each
# parser lives inside this module.
target_link_libraries(minimal_syntax_grammars PRIVATE
    tsgrammar_cpp
    tsgrammar_python
    tsgrammar_javascript
    tsgrammar_typescript
    tsgrammar_bash
    tsgrammar_json
    tsgrammar_yaml
    tsgrammar_rust
    tsgrammar_go)
