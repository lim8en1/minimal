// /memory -- the operator-side view of the store, mirroring the digest the
// model sees on its first memory call of a session, plus `purge <path>` for
// bulk removal of an ingest group. A command belongs to the plugin that owns
// the state it touches (the /restart-shell precedent).

#include <sstream>

#include "memory_tool.hpp"

#include "minimal/commands/command_context.hpp"

namespace minimal::memory {
namespace {

class MemoryCommand final : public commands::Command {
public:
    explicit MemoryCommand(std::shared_ptr<MemoryRuntime> runtime)
        : runtime_(std::move(runtime)) {}

    std::string_view Name() const override { return "memory"; }
    std::string_view Summary() const override { return "memory store status (digest)"; }
    std::string_view Usage() const override {
        return "/memory [purge <source path>] [reindex]";
    }
    bool RequiresSession() const override { return false; }
    bool ReadOnly() const override { return false; }  // purge/reindex write

    commands::CommandResult Execute(commands::CommandContext& context,
                                    const commands::CommandArgs& args) override {
        auto& rt = *runtime_;
        if (!args.positional.empty() && args.positional.front() == "purge") {
            if (args.positional.size() < 2) {
                return commands::CommandResult::Error("usage: /memory purge <source path>");
            }
            const std::string ref = args.positional[1];
            // Project-scope rows store the session's worktree root (see
            // StorePath), never "" -- purging with an empty project_root
            // matched zero rows and silently reported success. Without a
            // bound session there is no root to purge project-scope entries
            // under, so only the global scope is covered (same degrade the
            // digest below already has).
            int removed = rt.store->PurgeByOriginRef(ref, MemoryStore::kScopeGlobal, "");
            if (context.has_session()) {
                const std::string project_root =
                    WorktreeRoot(context.session().project_root()).string();
                removed += rt.store->PurgeByOriginRef(ref, MemoryStore::kScopeProject, project_root);
            }
            return commands::CommandResult::Ok("purged " + std::to_string(removed) +
                                               " entries from " + ref);
        }
        if (!args.positional.empty() && args.positional.front() == "reindex") {
            // The escape hatch MarkEmbedFailed's own comment promises: a row
            // that failed to embed (a transient backend outage, one
            // unserializable text) sits at embed_status=2 until either the
            // embedding signature changes or this runs -- there was no path
            // to the latter before this command existed, so a single bad
            // batch during an outage excluded those rows from semantic
            // search permanently.
            const int reset = rt.store->ResetFailedEmbeddings();
            return commands::CommandResult::Ok(
                "reindex: " + std::to_string(reset) + " failed row(s) queued for re-embedding");
        }

        // "" without a bound session (an operator may run /memory
        // sessionless): DigestInfo then reports zero project-scope entries
        // rather than every project's summed together.
        const std::string digest_project_root =
            context.has_session() ? WorktreeRoot(context.session().project_root()).string() : "";
        const Digest digest = rt.store->DigestInfo(
            rt.embedder ? rt.embedder->Describe() : "lexical only",
            rt.enricher->enabled() ? "on (" + rt.settings.summarizer_model + ")" : "off",
            digest_project_root);

        std::ostringstream out;
        out << "memory: ";
        bool first = true;
        for (const auto& [scope, count] : digest.entries_by_scope) {
            if (!first) out << ", ";
            out << scope << " " << count;
            first = false;
        }
        if (first) out << "empty";
        out << "\n  origin:";
        for (const auto& [origin, count] : digest.by_origin) out << " " << origin << "=" << count;
        out << "\n  backend: " << digest.backend;
        out << "\n  enrichment: " << digest.enrichment;
        if (digest.pending_reindex > 0) out << "\n  pending re-embed: " << digest.pending_reindex;
        const auto global_topics = rt.store->AllTopics(MemoryStore::kScopeGlobal, "");
        if (!global_topics.empty()) {
            out << "\n  global topics:";
            for (const auto& [name, count] : global_topics) out << " " << name << "(" << count << ")";
        }
        // Project topics too, when a session is bound. RequiresSession() is
        // false (an operator may run /memory sessionless), so this degrades
        // to the global-only view above rather than asserting on session().
        if (context.has_session()) {
            const std::string project_root =
                WorktreeRoot(context.session().project_root()).string();
            const auto project_topics =
                rt.store->AllTopics(MemoryStore::kScopeProject, project_root);
            if (!project_topics.empty()) {
                out << "\n  project topics:";
                for (const auto& [name, count] : project_topics) {
                    out << " " << name << "(" << count << ")";
                }
            }
        }
        return commands::CommandResult::Ok(out.str());
    }

private:
    std::shared_ptr<MemoryRuntime> runtime_;
};

}  // namespace

std::unique_ptr<commands::Command> MakeMemoryCommand(std::shared_ptr<MemoryRuntime> runtime) {
    return std::make_unique<MemoryCommand>(std::move(runtime));
}

}  // namespace minimal::memory
