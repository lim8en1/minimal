find_package(onnxruntime CONFIG REQUIRED)

minimal_add_plugin(memory
    plugin.cpp
    memory_settings.cpp
    embedding_http.cpp
    embedding_onnx.cpp
    hf_tokenizer.cpp
    enricher.cpp
    memory_store.cpp
    memory_tool.cpp
    memory_command.cpp)

# onnxruntime is a PRIVATE dep of this plugin only: header-only C++ wrapper over
# the C ABI, one shared libonnxruntime next to the other gathered runtime deps.
# Nothing else in the tree links it (see BUILD.md, Dependency linkage).
target_link_libraries(memory PRIVATE onnxruntime::onnxruntime)
