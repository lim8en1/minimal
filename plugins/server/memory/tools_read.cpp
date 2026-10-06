// memory_search and memory_get.

#include <algorithm>
#include <cmath>
#include <map>

#include "search.hpp"
#include "tool_common.hpp"
#include "policy.hpp"

namespace minimal::memory {

std::filesystem::path WorktreeRoot(const std::filesystem::path& project_root) {
    std::filesystem::path current = project_root;
    while (!current.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(current / ".git", ec)) return current;
        auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return project_root;
}

ScopeFilter VisibleFrom(const std::filesystem::path& session_root) {
    ScopeFilter filter;
    filter.project_root = WorktreeRoot(session_root).string();
    // A sessionless caller has no project: global rows only.
    filter.include_project = !filter.project_root.empty();
    return filter;
}

namespace {

using detail::Error;
using detail::Ok;


std::vector<std::string> StringArray(const nlohmann::json& input, const char* key) {
    std::vector<std::string> out;
    if (auto it = input.find(key); it != input.end() && it->is_array()) {
        for (const auto& value : *it) {
            if (value.is_string() && !value.get<std::string>().empty()) {
                out.push_back(value.get<std::string>());
            }
        }
    }
    return out;
}

class MemorySearchTool final : public tools::Tool {
public:
    explicit MemorySearchTool(std::shared_ptr<MemoryRuntime> runtime)
        : runtime_(std::move(runtime)) {
        spec_.name = "memory_search";
        spec_.category = tools::ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        // Says only what the model cannot infer: that memory spans sessions,
        // when to look, that browsing lists pinned entries and the whole tag
        // vocabulary (the context index may show only part of either), and
        // that results are summaries.
        spec_.definition.description =
            "Search memory saved in earlier sessions. Use it before re-deriving something that "
            "may already be settled, or when the user refers to past work. Omit `query` to "
            "browse: pinned entries first, then most recent, plus the full tag list. Returns "
            "summaries; memory_get has the bodies.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"query", {{"type", "string"}}},
              {"tags",
               {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "AND filter"}}},
              {"scope",
               {{"type", "string"},
                {"enum", {"all", "project", "global"}},
                {"description", "default all"}}},
              {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", kSearchLimitCeiling}}}}}};
    }

    const tools::ToolSpec& spec() const override { return spec_; }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        auto& rt = *runtime_;
        StartWorkers(rt);
        try {
            SearchRequest request;
            if (auto it = input.find("query"); it != input.end() && it->is_string()) {
                request.query = it->get<std::string>();
            }
            request.filters.scope = VisibleFrom(ctx.project_root);
            const std::string scope = input.value("scope", "all");
            if (scope == "project") request.filters.scope.include_global = false;
            else if (scope == "global") request.filters.scope.include_project = false;
            else if (scope != "all") return Error("scope must be \"all\", \"project\" or \"global\"");
            request.filters.tags = StringArray(input, "tags");
            request.limit = std::clamp(input.value("limit", rt.settings.default_limit), 1, kSearchLimitCeiling);

            const auto results = HybridSearch(*rt.store, rt.embeddings.get(), request);
            nlohmann::json list = nlohmann::json::array();
            for (const auto& result : results) {
                nlohmann::json j = detail::SummaryJson(result.row);
                if (request.query && !request.query->empty()) {
                    j["score"] = detail::RoundScore(result.score);
                }
                if (!result.snippet.empty()) j["match"] = result.snippet;
                list.push_back(std::move(j));
            }
            nlohmann::json body{{"results", list}};
            if (list.empty()) body["note"] = "no matches";
            if (!request.query || request.query->empty()) {
                // Browsing is how the model sees the WHOLE vocabulary: the
                // context index lists only the most-used tags.
                std::map<std::string, int> counts;
                const auto& scope_filter = request.filters.scope;
                if (scope_filter.include_project) {
                    for (const auto& tag : rt.store->Tags(kScopeProject, scope_filter.project_root)) {
                        counts[tag.name] += tag.count;
                    }
                }
                if (scope_filter.include_global) {
                    for (const auto& tag : rt.store->Tags(kScopeGlobal, "")) counts[tag.name] += tag.count;
                }
                if (!counts.empty()) body["tags"] = counts;
            }
            return Ok(body);
        } catch (const std::exception& error) {
            return Error(error.what());
        }
    }

private:
    std::shared_ptr<MemoryRuntime> runtime_;
    tools::ToolSpec spec_;
};

class MemoryGetTool final : public tools::Tool {
public:
    explicit MemoryGetTool(std::shared_ptr<MemoryRuntime> runtime) : runtime_(std::move(runtime)) {
        spec_.name = "memory_get";
        spec_.category = tools::ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description = "Full memory entries by id.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"ids",
               {{"type", "array"}, {"items", {{"type", "string"}}}, {"minItems", 1},
                {"maxItems", static_cast<int>(kGetMaxIds)}}}}},
            {"required", {"ids"}}};
    }

    const tools::ToolSpec& spec() const override { return spec_; }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        StartWorkers(*runtime_);
        try {
            auto ids = StringArray(input, "ids");
            // A lone "id" is the obvious mistake to be lenient about.
            if (ids.empty() && input.contains("id") && input["id"].is_string()) {
                ids.push_back(input["id"].get<std::string>());
            }
            if (ids.empty()) return Error("memory_get needs \"ids\"");
            if (ids.size() > kGetMaxIds) ids.resize(kGetMaxIds);
            const auto rows = runtime_->store->GetMany(ids, VisibleFrom(ctx.project_root));
            nlohmann::json entries = nlohmann::json::array();
            std::vector<std::string> missing = ids;
            for (const auto& row : rows) {
                entries.push_back(detail::FullJson(row));
                missing.erase(std::remove(missing.begin(), missing.end(), row.id), missing.end());
            }
            nlohmann::json body{{"entries", entries}};
            if (!missing.empty()) body["not_found"] = missing;
            return {rows.empty(), detail::Dump(body)};
        } catch (const std::exception& error) {
            return Error(error.what());
        }
    }

private:
    std::shared_ptr<MemoryRuntime> runtime_;
    tools::ToolSpec spec_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakeMemorySearchTool(std::shared_ptr<MemoryRuntime> runtime) {
    return std::make_unique<MemorySearchTool>(std::move(runtime));
}
std::unique_ptr<tools::Tool> MakeMemoryGetTool(std::shared_ptr<MemoryRuntime> runtime) {
    return std::make_unique<MemoryGetTool>(std::move(runtime));
}

}  // namespace minimal::memory
