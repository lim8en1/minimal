#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace minimal::client {

// The agent/model a user last picked, remembered per project so a new session
// starts where the previous one left off. Client state, not configuration: it
// lives in the data dir (`last_selection.json`, beside prompt_history.json),
// never in the user-owned config dir.
//
//   {"projects": {"<project root>": {"agent": "plan", "model": "local/qwen4"}},
//    "global":   {"agent": "plan", "model": "local/qwen4"}}
//
// Every pick also updates "global". Per field, a project's own pick wins and
// an absent field falls back to the global one; "model": "" is an explicit
// pick of the default (settings.json default_model).
struct LastSelection {
    std::string agent;
    std::string model;
};

class LastSelectionStore {
public:
    explicit LastSelectionStore(std::filesystem::path file);

    // The project's entry, else the global one, else empty. A missing or
    // malformed file reads as empty (it is the client's own cache; a broken
    // one costs a re-pick, never a failed start).
    LastSelection For(const std::string& project_root) const;

    // Records a pick for the project and globally. nullopt leaves that field
    // as it was; an empty model is the explicit "back to the default".
    void Record(const std::string& project_root, const std::optional<std::string>& agent,
                const std::optional<std::string>& model) const;

    // Drops remembered fields the server refused (a preset deleted, a
    // provider removed), from the project entry and the global one.
    void Forget(const std::string& project_root, bool agent, bool model) const;

private:
    std::filesystem::path file_;
};

}  // namespace minimal::client
