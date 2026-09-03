#pragma once

// `<provider>/<model>` — how a model is named, everywhere it is named.
//
// In common/ rather than server-side config/ because BOTH halves name models:
// the server resolves one against providers.json, and the client accepts one
// from `--model` (and later `/model`). Validating in one place is what keeps the
// client from accepting something the server will reject, or from inventing a
// second, subtly different idea of what a valid reference looks like.
//
// This is the same syntax in four positions — agents/*.md `model:`,
// settings.json `default_model`, `/model`, `--model` — so there is one parser.

#include <optional>
#include <string>
#include <string_view>

namespace minimal::common {

struct ModelRef {
    std::string provider;
    std::string model;

    std::string str() const { return provider + "/" + model; }
};

// nullopt when the text is not exactly one '/' separating two non-empty halves.
// Rejects "qwen4" (no provider), "local/" and "/qwen4" (an empty half), and
// "a/b/c" — a name containing a slash makes the split a guess, and taking the
// first one silently yields a provider called "a".
std::optional<ModelRef> ParseModelRef(std::string_view text);

// The interactive-path split: everything after the FIRST '/' is the model.
// Discovered catalogs legitimately serve model ids that contain slashes --
// OpenRouter's "anthropic/claude-opus-5" under provider "openrouter" makes
// the full ref three segments -- and strict parsing would make those
// unaddressable from every surface that names a model interactively
// (--model, /model, the picker). The wrong-split risk strict parsing guards
// against does not exist here: a mis-split ref resolves to nothing and is
// reported as "unknown model", which is the honest outcome for a typo.
// nullopt when either half would be empty.
std::optional<ModelRef> SplitModelRef(std::string_view text);

}  // namespace minimal::common
