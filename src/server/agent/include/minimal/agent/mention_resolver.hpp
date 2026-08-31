#pragma once

// @-mention path SUGGESTION (ARCHITECTURE.md "@-mention file references").
//
// The strategy is pluggable and lives server-side so every frontend inherits it
// -- that is the whole reason SuggestPaths is an RPC rather than client code.
// EXPANSION is deliberately not routed through here: it is a deterministic
// scan+inline (mention_expansion.hpp), not a strategy someone might want fuzzy.
//
// Both ends of this file are reachable from the plugin ABI:
// minimal_plugin_register_mention_resolver(MentionResolverRegistry&) -- see
// plugin_abi.h.

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "minimal/common/logger.hpp"

namespace minimal::agent {

class MentionResolver {
public:
    virtual ~MentionResolver() = default;

    // Paths relative to project_root, at most `limit` of them. Directories carry
    // a trailing '/' so a client can descend without a stat round-trip -- the
    // same convention bash completion uses.
    //
    // A prefix naming no readable directory yields an empty list. A bad prefix
    // is a normal state while typing, not an error; there is no error channel.
    virtual std::vector<std::string> Suggest(std::string_view prefix,
                                             const std::filesystem::path& project_root,
                                             int limit) const = 0;
};

// Core default: case-sensitive prefix match listing EXACTLY ONE directory --
// the deepest complete component in the prefix -- precisely like bash path
// completion. "src" lists root entries starting with "src"; "src/" lists src's
// entries; "src/ma" still lists src's entries. Descent happens only when the
// user types '/'.
//
// It never walks recursively. A repo with node_modules/.git would make every
// debounced keystroke expensive, against the low-CPU goal; fuzzy or recursive
// matching is what a plugin override is for, and is deliberately not built in.
class PrefixMentionResolver final : public MentionResolver {
public:
    std::vector<std::string> Suggest(std::string_view prefix,
                                     const std::filesystem::path& project_root,
                                     int limit) const override;
};

// Exclusive: ONE strategy is in force. First plugin (plugins.json order) wins;
// a later registration logs a warning and is ignored -- two plugins fighting
// over the suggestion box would make which one you get depend on load order,
// which is the failure mode the collision rules exist to prevent everywhere else.
class MentionResolverRegistry {
public:
    explicit MentionResolverRegistry(common::Logger& logger);

    // Startup-only in production (plugin load), but mutex-guarded so a test or
    // a future hot path cannot turn that assumption into a data race.
    void Register(std::shared_ptr<MentionResolver> resolver);

    // The plugin override, or the core default when none registered. Never null.
    const MentionResolver& Active() const;
    bool has_override() const;

private:
    mutable std::mutex mutex_;
    PrefixMentionResolver default_;
    std::shared_ptr<MentionResolver> override_;
    common::Logger& logger_;
};

}  // namespace minimal::agent
