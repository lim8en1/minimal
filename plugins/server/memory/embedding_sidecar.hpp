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

// Spawns the sidecar and waits for its ready handshake. Throws
// std::runtime_error naming the reason (python not found, spawn failure,
// fastembed import/model-load failure): the EmbeddingService logs it, trips
// its breaker and reports it through /memory -- never a silent null. Called
// on the embedding worker, never at plugin registration (a first run may
// download model weights). `data_dir` is the plugin's own storage dir; the
// fastembed model cache lives under it rather than the user's home cache.
std::unique_ptr<EmbeddingFunction> MakeSidecarEmbeddingFunction(
    const MemorySettings& settings, const std::filesystem::path& data_dir);

// "sidecar:<model>", without spawning anything.
std::string SidecarSignatureFor(const MemorySettings& settings);

}  // namespace minimal::memory
