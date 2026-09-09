#pragma once

// `<provider>/<model>` — how a model is named, everywhere it is named.
//
// In common/ rather than server-side config/ because BOTH halves name models:
// the server resolves one against providers.json, and the client accepts one
// from `--model` (and later `/model`). Validating in one place is what keeps the
// client from accepting something the server will reject, or from inventing a
// second, subtly different idea of what a valid reference looks like.
//
// This is the same syntax in five positions — agents/*.md `model:`,
// settings.json `default_model`/`small_model`, `/model`, `--model`, and a
// plugin config's model override (session-titler) — and all five go through
// SplitModelRef, below, so a model discoverable through any one of them is
// never rejected by another. ParseModelRef (the strict form) has no
// production caller as of this writing: every surface that used to reject
// slash-bearing discovered model ids (e.g. an OpenRouter model addressed as
// "openrouter/anthropic/claude-opus-5") was that asymmetry, and has been
// moved onto SplitModelRef. It stays here, tested directly by
// tests/model_ref_test.cpp and tests/providers_config_test.cpp, as the
// stricter primitive for a future caller that genuinely needs to reject a
// name with more than one '/' rather than resolve it to "unknown model".

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
