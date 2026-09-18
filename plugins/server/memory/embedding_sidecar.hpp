#pragma once

// Sidecar backend (embedding_sidecar.cpp): a long-lived python subprocess
// running fastembed, replacing the earlier in-process onnx backend -- see
// ARCHITECTURE.md / the memory plugin's README for why (no other plugin in
// this codebase links a native ML runtime; every other "local inference"
// story here is an HTTP client pointed at a locally-run server, and fastembed
// ships onnxruntime as a prebuilt PyPI wheel, sidestepping this repo's
// vcpkg/onnxruntime build problem entirely).

#include <filesystem>
#include <memory>

#include "embedding.hpp"
#include "memory_settings.hpp"

namespace minimal::memory {

// Null on any failure -- python not found, spawn failure, fastembed
// import/model-load failure -- so the plugin loads and the strategy degrades
// to lexical (the tools self-describe through /memory). `data_dir` is the
// plugin's own storage dir; the sidecar's fastembed model cache lives under
// it rather than the user's home cache.
std::unique_ptr<EmbeddingFunction> TryMakeSidecarEmbeddingFunction(
    const MemorySettings& settings, const std::filesystem::path& data_dir);

}  // namespace minimal::memory
