// /memory -- the operator's view of and control over the store. A command
// belongs to the plugin that owns the state it touches (the /restart-shell
// precedent).
//
//   /memory                               status: counts, backends, queues
//   /memory index                         the Memory section a session here gets
//   /memory tags [global]                 the vocabulary with counts
//   /memory tags merge <from> <to> [global]
//   /memory tags rename <from> <to> [global]
//   /memory tags suggest-merges [global]  ask the tagger model; apply by hand
//   /memory purge <path|group-id>         remove every entry of one import
//   /memory reindex                       re-queue entries that failed to embed
//
// Tag commands act on this project's vocabulary unless "global" is given.

#include <filesystem>
#include <sstream>

#include "memory_runtime.hpp"

#include "minimal/commands/command_context.hpp"

namespace minimal::memory {
namespace {

class MemoryCommand final : public commands::Command {
public:
    explicit MemoryCommand(std::shared_ptr<MemoryRuntime> runtime) : runtime_(std::move(runtime)) {}

    std::string_view Name() const override { return "memory"; }
    std::string_view Summary() const override { return "memory status, tags, purge, reindex"; }
    std::string_view Usage() const override {
        return "/memory [index | tags [merge|rename <from> <to> | suggest-merges] [global] | "
               "purge <path|group-id> | reindex]";
    }
    bool RequiresSession() const override { return false; }
    bool ReadOnly() const override { return false; }  // tags/purge/reindex write

    commands::CommandResult Execute(commands::CommandContext& context,
                                    const commands::CommandArgs& args) override {
        const std::filesystem::path session_root =
            context.has_session() ? context.session().project_root() : std::filesystem::path();
        const auto& p = args.positional;
        StartWorkers(*runtime_);
        try {
            if (p.empty()) return Status(session_root);
            if (p[0] == "index") return commands::CommandResult::Ok(RenderMemoryIndex(*runtime_, session_root));
            if (p[0] == "tags") return Tags(session_root, p);
            if (p[0] == "purge") return Purge(session_root, p);
            if (p[0] == "reindex") {
                const int reset = runtime_->store->ResetFailedEmbeddings();
                runtime_->embeddings->Notify();
                return commands::CommandResult::Ok("reindex: " + std::to_string(reset) +
                                                   " failed entr(ies) queued for embedding");
            }
            return commands::CommandResult::Error(std::string("usage: ") + std::string(Usage()));
        } catch (const std::exception& error) {
            return commands::CommandResult::Error(std::string("memory: ") + error.what());
        }
    }

private:
    commands::CommandResult Status(const std::filesystem::path& session_root) {
        auto& rt = *runtime_;
        const ScopeFilter filter = VisibleFrom(session_root);
        const StoreStats stats = rt.store->Stats(filter);
        std::ostringstream out;
        out << "memory:";
        if (stats.entries_by_scope.empty()) out << " empty";
        for (const auto& [scope, count] : stats.entries_by_scope) out << " " << scope << " " << count;
        if (stats.pinned > 0) out << " (" << stats.pinned << " pinned)";
        if (!stats.by_origin.empty()) {
            out << "\n  origin:";
            for (const auto& [origin, count] : stats.by_origin) out << " " << origin << "=" << count;
        }
        out << "\n  embeddings: " << rt.embeddings->Describe();
        if (stats.pending_embed > 0) out << "\n  pending embedding: " << stats.pending_embed;
        if (stats.failed_embed > 0) {
            out << "\n  failed embedding: " << stats.failed_embed << " (/memory reindex retries)";
        }
        out << "\n  tagging: " << rt.tagger->Describe();
        if (stats.pending_tagging > 0 && rt.tagger->enabled()) {
            out << "\n  pending tagging: " << stats.pending_tagging;
        }
        out << "\n  tags: " << stats.tags;
        if (!filter.include_project) out << "\n  (no session: project entries not shown)";
        return commands::CommandResult::Ok(out.str());
    }

    // Trailing "global" selects the global vocabulary.
    static bool WantsGlobal(const std::vector<std::string>& p) {
        return !p.empty() && p.back() == "global";
    }

    commands::CommandResult Tags(const std::filesystem::path& session_root,
                                 const std::vector<std::string>& p) {
        auto& rt = *runtime_;
        const bool global = WantsGlobal(p);
        const ScopeFilter filter = VisibleFrom(session_root);
        if (!global && !filter.include_project) {
            return commands::CommandResult::Error(
                "no session is bound, so there is no project vocabulary; add \"global\"");
        }
        const int scope = global ? kScopeGlobal : kScopeProject;
        const std::string root = global ? std::string() : filter.project_root;
        const std::string label = global ? "global" : "project";
        const std::size_t argc = p.size() - (global ? 1 : 0);

        if (argc == 1) {
            const auto tags = rt.store->Tags(scope, root);
            if (tags.empty()) return commands::CommandResult::Ok("no " + label + " tags");
            std::ostringstream out;
            out << label << " tags (" << tags.size() << "/" << rt.settings.max_tags << "):";
            for (const auto& tag : tags) {
                out << "\n  " << tag.name << " (" << tag.count << ")";
                if (!tag.description.empty()) out << " -- " << tag.description;
            }
            return commands::CommandResult::Ok(out.str());
        }
        const std::string& verb = p[1];
        if ((verb == "merge" || verb == "rename") && argc == 4) {
            const bool ok = verb == "merge" ? rt.store->MergeTags(scope, root, p[2], p[3])
                                            : rt.store->RenameTag(scope, root, p[2], p[3]);
            if (!ok) {
                return commands::CommandResult::Error(
                    verb == "merge" ? "merge needs two different existing " + label + " tags"
                                    : "rename needs an existing " + label +
                                          " tag and a name no other tag uses (that would be a merge)");
            }
            return commands::CommandResult::Ok(verb + "d " + p[2] + " -> " + p[3]);
        }
        if (verb == "suggest-merges" && argc == 2) {
            const auto suggestions = rt.tagger->SuggestMerges(scope, root);
            if (suggestions.empty()) return commands::CommandResult::Ok("no merges suggested");
            std::ostringstream out;
            out << "suggested merges (apply with /memory tags merge <from> <to>"
                << (global ? " global" : "") << "):";
            for (const auto& s : suggestions) {
                out << "\n  " << s.from << " -> " << s.to;
                if (!s.why.empty()) out << "  (" << s.why << ")";
            }
            return commands::CommandResult::Ok(out.str());
        }
        return commands::CommandResult::Error(
            "usage: /memory tags [merge|rename <from> <to> | suggest-merges] [global]");
    }

    commands::CommandResult Purge(const std::filesystem::path& session_root,
                                  const std::vector<std::string>& p) {
        if (p.size() < 2) return commands::CommandResult::Error("usage: /memory purge <path|group-id>");
        std::string target = p[1];
        // Imports are keyed by absolute path; accept one relative to the
        // session's root, as the operator would type it.
        std::filesystem::path as_path(target);
        if (as_path.is_relative() && !session_root.empty() && target.rfind("ig", 0) != 0) {
            std::error_code ec;
            const auto absolute = std::filesystem::weakly_canonical(session_root / as_path, ec);
            if (!ec) target = absolute.string();
        }
        const int removed = runtime_->store->DeleteGroup(target, VisibleFrom(session_root));
        if (removed == 0) return commands::CommandResult::Error("no import found for " + p[1]);
        return commands::CommandResult::Ok("purged " + std::to_string(removed) + " entries from " + p[1]);
    }

    std::shared_ptr<MemoryRuntime> runtime_;
};

class MemoryUnavailableCommand final : public commands::Command {
public:
    explicit MemoryUnavailableCommand(std::string reason) : reason_(std::move(reason)) {}
    std::string_view Name() const override { return "memory"; }
    std::string_view Summary() const override { return "memory is unavailable (see reason)"; }
    std::string_view Usage() const override { return "/memory"; }
    bool RequiresSession() const override { return false; }
    bool ReadOnly() const override { return true; }
    commands::CommandResult Execute(commands::CommandContext&, const commands::CommandArgs&) override {
        return commands::CommandResult::Error("memory is unavailable: " + reason_);
    }

private:
    std::string reason_;
};

}  // namespace

std::unique_ptr<commands::Command> MakeMemoryUnavailableCommand(std::string reason) {
    return std::make_unique<MemoryUnavailableCommand>(std::move(reason));
}

std::unique_ptr<commands::Command> MakeMemoryCommand(std::shared_ptr<MemoryRuntime> runtime) {
    return std::make_unique<MemoryCommand>(std::move(runtime));
}

}  // namespace minimal::memory
