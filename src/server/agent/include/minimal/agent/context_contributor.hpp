#pragma once

// Context contributors: plugins that PRE-FILL a session's system context.
//
// A contributor is asked once per session, in the SessionRegistry funnel every
// session passes through (top-level creation, resume, subagent spawn), right
// after AGENTS.md and the signal-channel context. Its section is appended to
// the same system-prompt tail and FROZEN for the session: the request prefix
// the provider's prompt cache keys on never shifts mid-session. A resume asks
// again, so a contributor whose content changed between runs is picked up at
// the next natural cache break.
//
// Reachable from the plugin ABI:
// minimal_plugin_register_context_contributors(ContextContributorRegistry&) --
// see plugin_abi.h. The memory plugin's index is the first consumer; nothing
// here knows about it.
//
// CONTRACT for implementers:
//   - Contribute() runs SYNCHRONOUSLY on the session-creation path (a gRPC
//     handler or a subagent spawn). Keep it cheap: read local state, no
//     network, no provider calls. It blocks the session from starting.
//   - Throwing is contained: the registry logs and skips that contributor.
//   - nullopt, or an empty body, contributes nothing (the normal state).
//   - The body is capped by settings.json context_contributors
//     .max_bytes_per_section; an over-budget body is cut at a UTF-8 boundary
//     with a visible marker. Budget your own content; the cap is a backstop.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "minimal/common/logger.hpp"
#include "minimal/config/settings_config.hpp"

namespace minimal::agent {

struct ContextRequest {
    std::string session_id;
    std::string parent_session_id;  // empty for a top-level session
    std::filesystem::path project_root;
    std::string preset_name;
    bool is_subagent = false;
    bool resume = false;
};

struct ContextSection {
    std::string title;  // rendered as a "## <title>" heading
    std::string body;
};

// Where sections land in the session's context tail: ascending priority, 0
// first; equal priorities keep registration (plugins.json load) order. Core's
// own signal-channel instructions sit at kSignalContextPriority.
inline constexpr int kDefaultContextPriority = 100;
inline constexpr int kSignalContextPriority = 50;

class ContextContributor {
public:
    virtual ~ContextContributor() = default;
    virtual std::optional<ContextSection> Contribute(const ContextRequest& request) = 0;
    virtual int priority() const { return kDefaultContextPriority; }
    // This section's own size cap in bytes (0 = uncapped), replacing the
    // operator's generic max_bytes_per_section -- for a contributor whose
    // size is configured where it is defined (project-context's max_bytes).
    // nullopt: the generic cap applies.
    virtual std::optional<std::int64_t> max_bytes() const { return std::nullopt; }
};

// One rendered section and where it goes.
struct RenderedContextSection {
    int priority = kDefaultContextPriority;
    std::string text;
};

// Joins sections in priority order (stable), "\n\n" between them.
std::string JoinContextSections(std::vector<RenderedContextSection> sections);

// settings.json context_contributors: {disabled: [plugin names],
// max_bytes_per_section}.
using ContextContributorSettings = config::ContextContributorSettings;

// Additive: every plugin may register any number of contributors. Sections
// render by declared priority, then registration order (plugins.json load
// order) -- deterministic either way, the prompt-cache argument.
class ContextContributorRegistry {
public:
    explicit ContextContributorRegistry(common::Logger& logger);

    ContextContributorRegistry(const ContextContributorRegistry&) = delete;
    ContextContributorRegistry& operator=(const ContextContributorRegistry&) = delete;

    // Attribution window, the loader's shape for tools/commands: every
    // contributor registered while it is open belongs to `plugin_name`, which
    // is what `disabled` matches and what a failure log names.
    void SetAttribution(std::string plugin_name);
    void ClearAttribution();

    // Startup-only in production (plugin load). A contributor registered
    // outside an attribution window is attributed to "core".
    void Register(std::shared_ptr<ContextContributor> contributor);

    // Registration-time rollback for a plugin whose hook threw. Safe only
    // before any session exists.
    void RemoveByAttribution(const std::string& plugin_name);
    std::size_t CountOwnedBy(const std::string& plugin_name) const;
    std::size_t count() const;

    void Configure(ContextContributorSettings settings);

    // Every enabled contributor's section, rendered, in registration order
    // with its priority (the caller merges in core sections and orders).
    std::vector<RenderedContextSection> RenderSections(const ContextRequest& request) const;
    // RenderSections, ordered and joined; "" when none contributed.
    std::string Render(const ContextRequest& request) const;

private:
    struct Entry {
        std::string plugin;
        std::shared_ptr<ContextContributor> contributor;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::string attribution_;
    ContextContributorSettings settings_;
    common::Logger& logger_;
};

}  // namespace minimal::agent
