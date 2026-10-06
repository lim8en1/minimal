// The "Memory" section pre-filled into each session's context (the core
// context-contributor seam), so the model knows what memory holds before it
// decides whether to look.
//
// ONE byte budget (index_budget_bytes), filled in priority order:
//   1. pinned entries -- full bodies, then one-liners -- up to half of it;
//   2. the tag vocabulary, most-used first;
//   3. recently updated entries as one-liners.
// Every section that had to be trimmed SAYS so, with a count and the call
// that fetches the rest: the model must never have to guess that more
// exists. Tool usage guidance is deliberately absent -- it lives in the tool
// descriptions, which are present whenever this section is (not vice versa:
// subagents get the tools without the index).
//
// Rendered once per session and frozen with the rest of the system context.

#include <algorithm>
#include <map>

#include "memory_runtime.hpp"
#include "policy.hpp"

namespace minimal::memory {
namespace {


std::string OneLiner(const EntryRow& row) {
    return "- " + row.title + " -- " + row.description +
           (row.scope == kScopeGlobal ? " (global)" : "") + "\n";
}

class MemoryIndexContributor final : public agent::ContextContributor {
public:
    explicit MemoryIndexContributor(std::shared_ptr<MemoryRuntime> runtime)
        : runtime_(std::move(runtime)) {}

    std::optional<agent::ContextSection> Contribute(const agent::ContextRequest& request) override {
        auto& rt = *runtime_;
        StartWorkers(rt);
        if (!rt.settings.index_enabled) return std::nullopt;
        if (request.is_subagent && !rt.settings.index_in_subagents) return std::nullopt;
        return agent::ContextSection{"Memory", RenderMemoryIndex(rt, request.project_root)};
    }

private:
    std::shared_ptr<MemoryRuntime> runtime_;
};

}  // namespace

std::string RenderMemoryIndex(MemoryRuntime& rt, const std::filesystem::path& session_root) {
    const ScopeFilter filter = VisibleFrom(session_root);
    const StoreStats stats = rt.store->Stats(filter);
    const int project_count =
        stats.entries_by_scope.count("project") ? stats.entries_by_scope.at("project") : 0;
    const int global_count =
        stats.entries_by_scope.count("global") ? stats.entries_by_scope.at("global") : 0;
    if (project_count + global_count == 0) return "No memories stored yet.\n";

    const std::size_t budget = static_cast<std::size_t>(rt.settings.index_budget_bytes);
    std::string out = std::to_string(project_count) + " project and " +
                      std::to_string(global_count) + " global entries from earlier sessions.\n";

    // 1. Pinned: full bodies while they fit in half the budget, one-liners
    //    after that, and a count for whatever is left.
    const auto pinned = rt.store->Pinned(filter);
    if (!pinned.empty()) {
        const std::size_t pinned_budget = budget / 2;
        std::string block = "\nPinned (standing facts and preferences):\n";
        int omitted = 0;
        for (const auto& row : pinned) {
            const std::string full = "### " + row.title +
                                     (row.scope == kScopeGlobal ? " (global)" : "") + "\n" +
                                     row.body + "\n";
            const std::string brief = OneLiner(row);
            if (block.size() + full.size() <= pinned_budget) {
                block += full;
            } else if (block.size() + brief.size() <= pinned_budget) {
                block += brief;  // body omitted: memory_search/get has it
            } else {
                ++omitted;
            }
        }
        if (omitted > 0) {
            block += "(+" + std::to_string(omitted) +
                     " more pinned; memory_search with no query lists pinned entries first.)\n";
        }
        out += block;
    }

    // 2. Tags, most-used first; project and global vocabularies merged (a tag
    //    filter matches by name in either scope).
    std::map<std::string, int> counts;
    if (filter.include_project) {
        for (const auto& tag : rt.store->Tags(kScopeProject, filter.project_root)) counts[tag.name] += tag.count;
    }
    for (const auto& tag : rt.store->Tags(kScopeGlobal, "")) counts[tag.name] += tag.count;
    if (!counts.empty()) {
        std::vector<std::pair<std::string, int>> tags(counts.begin(), counts.end());
        std::stable_sort(tags.begin(), tags.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });
        const std::size_t tag_budget = out.size() + (budget > out.size() ? (budget - out.size()) / 2 : 0);
        std::string line = "\nTags:";
        std::size_t shown = 0;
        for (const auto& [name, count] : tags) {
            const std::string item = " " + name + "(" + std::to_string(count) + ")";
            if (out.size() + line.size() + item.size() > tag_budget) break;
            line += item;
            ++shown;
        }
        if (shown < tags.size()) {
            line += " (+" + std::to_string(tags.size() - shown) +
                    " more; memory_search with no query lists all tags)";
        }
        out += line + "\n";
    }

    // 3. Recent one-liners with what is left.
    const int unpinned_total = project_count + global_count - static_cast<int>(pinned.size());
    if (unpinned_total > 0) {
        std::string lines;
        int shown = 0;
        for (const auto& row : rt.store->Recent(filter, kRecentCandidates)) {
            const std::string line = OneLiner(row);
            if (out.size() + lines.size() + line.size() + kIndexHeaderRoomBytes > budget) break;
            lines += line;
            ++shown;
        }
        if (shown == unpinned_total) {
            out += "\nEntries:\n" + lines;
        } else {
            out += "\nRecent entries (" + std::to_string(shown) + " of " +
                   std::to_string(unpinned_total) + "; memory_search finds the rest):\n" + lines;
        }
    }
    return out;
}

std::shared_ptr<agent::ContextContributor> MakeMemoryIndexContributor(
    std::shared_ptr<MemoryRuntime> runtime) {
    return std::make_shared<MemoryIndexContributor>(std::move(runtime));
}

}  // namespace minimal::memory
