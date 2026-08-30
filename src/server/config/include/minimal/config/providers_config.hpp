#pragma once

// providers.json: what backends exist, and which models each one serves.
//
// A PROVIDER IS A CONNECTION, NOT A MODEL, and that split is the whole point of
// this file's shape. The previous schema made an entry *be* a model -- name,
// base_url and model_name in one flat record -- which meant two models on one
// endpoint had to duplicate the connection, and `parameters`/`max_tokens` came
// from the entry no matter which model a preset had selected. One llama.cpp
// serving a 128k model and a 32k model was inexpressible: there was nowhere to
// put the second context window.
//
//   {
//     "providers": {
//       "local": {
//         "backend": "openai",
//         "url": "http://localhost:8000/v1",
//         "max_tokens": 4096,                       <- default for every model here
//         "parameters": { "temperature": 0.3 },
//         "models": {
//           "qwen4": { "context_window_tokens": 131072 },
//           "small": { "context_window_tokens": 32768, "max_tokens": 1024 }
//         }
//       }
//     }
//   }
//
// A MAP, not an array: the key IS the provider name, so it cannot disagree with
// a `name` field sitting beside it. Models nest the same way for the same reason.
//
// Addressed as `<provider>/<model>` -- "local/qwen4" -- everywhere a model is
// named: preset frontmatter, settings.default_model, /model, --model. One syntax,
// so a name that works in one place works in all of them.
//
// `backend` names a ProviderFactoryRegistry kind, looked up at session
// construction and not validated here: any plugin-registered kind is legal, so
// there is no closed enum to keep in sync.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/common/model_ref.hpp"
#include "minimal/llm/provider.hpp"

namespace minimal::config {

// Both live in common/, because the CLIENT names models too (--model) and must
// not accept something the server would reject. Re-exported here so server-side
// code reads in one namespace.
using common::ModelRef;
using common::ParseModelRef;

// One model, with its provider's connection and its inherited settings already
// merged in. This is what a session needs to construct a provider and issue a
// request; nothing downstream has to know the file's shape.
struct ResolvedModel {
    // Its own name. Two providers may point at the same url and serve the same
    // model id -- one capped, one not -- so the connection and the model id
    // together do NOT identify which reference a session is running. Only this
    // does, and /providers needs to say which line is the active one.
    ModelRef ref;
    std::string backend;           // ProviderFactoryRegistry kind
    llm::ProviderConfig provider;  // url, api_key, model_name, timeouts
    // Provider-level values with the model's own layered on top, resolved at
    // LOAD rather than per lookup -- so the merged result is inspectable in one
    // place and every caller sees the same answer.
    nlohmann::json parameters = nlohmann::json::object();
    std::optional<int> max_tokens;
    std::optional<std::int64_t> context_window_tokens;
};

class ProvidersConfig {
public:
    // nullptr when the provider or the model within it is unknown. Callers
    // report; this layer does not know whether a miss is a typo in a preset, in
    // settings.json, or in something a user just typed.
    const ResolvedModel* Resolve(const ModelRef& ref) const;

    bool empty() const { return models_.empty(); }

    // Every `<provider>/<model>` this config defines, sorted. What `/providers`
    // lists and what a "no such model" error offers as alternatives.
    std::vector<std::string> ModelRefs() const;

    // Insertion is parse-time only; exposed so ParseProvidersConfig can build one
    // without friending it, and so tests can construct a config directly.
    void Add(const ModelRef& ref, ResolvedModel resolved);

private:
    // Ordered, so ModelRefs() is sorted without a second pass and two runs list
    // providers identically.
    std::map<std::string, ResolvedModel> models_;  // key: "<provider>/<model>"
};

// Pure. Throws std::runtime_error on: a top-level array (the pre-`models` schema
// -- refused by name rather than silently misparsed), a provider missing
// `backend`/`url`, a provider with no `models`, both api_key and api_key_env set,
// or api_key_env naming an unset variable. Neither key set is NOT an error --
// local servers routinely need none.
ProvidersConfig ParseProvidersConfig(const nlohmann::json& doc);

// Thin: reads the file and parses it. Throws if the file is missing or is not
// valid JSON, or per ParseProvidersConfig above.
ProvidersConfig LoadProvidersConfig(const std::filesystem::path& path);

}  // namespace minimal::config
