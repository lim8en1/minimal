#pragma once

// `<provider>/<model>` — how a model is named, everywhere it is named.
//
// In common/ rather than server-side config/ because BOTH halves name models:
// the server resolves one against providers.json, and the client accepts one
// from `--model` (and later `/model`). Validating in one place is what keeps the
// client from accepting something the server will reject, or from inventing a
// second, subtly different idea of what a valid reference looks like.
//
// The same syntax in five positions -- agents/*.md `model:`, settings.json
// `default_model`/`small_model`, `/model`, `--model`, and a plugin config's
// model override -- and all five go through SplitModelRef, so a model
// addressable through one is never rejected by another.

#include <optional>
#include <string>
#include <string_view>

namespace minimal::common {

struct ModelRef {
    std::string provider;
    std::string model;

    std::string str() const { return provider + "/" + model; }
};

// Everything after the FIRST '/' is the model: discovered catalogs serve ids
// that contain slashes (OpenRouter's "anthropic/claude-opus-5" under provider
// "openrouter"). A mis-split ref resolves to nothing and is reported as
// "unknown model", the honest outcome for a typo. Surrounding whitespace on
// either half is trimmed. nullopt when either half would be empty.
std::optional<ModelRef> SplitModelRef(std::string_view text);

}  // namespace minimal::common
