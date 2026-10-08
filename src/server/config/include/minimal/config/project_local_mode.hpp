#pragma once

// Split out of settings_config.hpp: the SDK's project_trust.hpp needs this
// enum, not the whole settings model.

namespace minimal::config {

// Whether the project's own agents/, skills/ and commands/ directories (found by
// walking up from the session's project root to its git root) are loaded. They
// are content a cloned repository ships, so by default they load only for
// directories the operator has trusted (`/trust-directory`). AGENTS.md /
// CLAUDE.md is the project-context plugin's, and not governed by this.
enum class ProjectLocalMode {
    kTrusted,  // load for trusted directories only (the default)
    kAll,      // load for every project (the behaviour before the trust gate)
    kNone,     // never load project-local content
};

}  // namespace minimal::config
