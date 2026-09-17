// /memory -- the operator-side view of the store, mirroring the digest the
// model sees on its first memory call of a session, plus `purge <path>` for
// bulk removal of an ingest group. A command belongs to the plugin that owns
// the state it touches (the /restart-shell precedent).

#include <sstream>

#include "memory_tool.hpp"

namespace minimal::memory {
namespace {

class MemoryCommand final : public commands::Command {
public:
    explicit MemoryCommand(std::shared_ptr<MemoryRuntime> runtime)
        : runtime_(std::move(runtime)) {}

    std::string_view Name() const override { return "memory"; }
    std::string_view Summary() const override { return "memory store status (digest)"; }
    std::string_view Usage() const override { return "/memory [purge <source path>]"; }
    bool RequiresSession() const override { return false; }
    bool ReadOnly() const override { return false; }  // purge writes

    commands::CommandResult Execute(commands::CommandContext& context,
                                    const commands::CommandArgs& args) override {
        auto& rt = *runtime_;
        if (!args.positional.empty() && args.positional.front() == "purge") {
            if (args.positional.size() < 2) {
                return commands::CommandResult::Error("usage: /memory purge <source path>");
            }
            const std::string ref = args.positional[1];
            const int removed =
                rt.store->PurgeByOriginRef(ref, MemoryStore::kScopeGlobal, "") +
                rt.store->PurgeByOriginRef(ref, MemoryStore::kScopeProject, "");
            return commands::CommandResult::Ok("purged " + std::to_string(removed) +
                                               " entries from " + ref);
        }

        const Digest digest = rt.store->DigestInfo(
            rt.embedder ? rt.embedder->Describe() : "lexical only",
            rt.enricher->enabled() ? "on (" + rt.settings.summarizer_model + ")" : "off");

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
        const auto topics = rt.store->AllTopics(MemoryStore::kScopeGlobal, "");
        if (!topics.empty()) {
            out << "\n  global topics:";
            for (const auto& [name, count] : topics) out << " " << name << "(" << count << ")";
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
