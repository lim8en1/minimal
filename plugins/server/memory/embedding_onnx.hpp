#pragma once

#include <filesystem>
#include <memory>

#include "embedding.hpp"
#include "memory_settings.hpp"

namespace minimal::memory {

// Loads the model + tokenizer.json (paths relative to data_dir unless
// absolute). Returns null on ANY failure -- missing files, unparsable
// tokenizer, session load error -- so the plugin loads and the strategy
// degrades to lexical (the tools self-describe through /memory).
std::unique_ptr<EmbeddingFunction> TryMakeOnnxEmbeddingFunction(
    const MemorySettings& settings, const std::filesystem::path& data_dir);

}  // namespace minimal::memory
