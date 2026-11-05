// memory_store, memory_update and memory_delete.

#include <cmath>
#include <fstream>
#include <iterator>

#include "ingest.hpp"
#include "search.hpp"
#include "tool_common.hpp"

#include "minimal/common/utf8.hpp"
#include "minimal/tools/path_confinement.hpp"
#include "minimal/common/units.hpp"
#include "policy.hpp"
#include "minimal/tools/policy.hpp"

namespace minimal::memory {
namespace {

using detail::Error;
using detail::Ok;


// Titles and descriptions are single lines: they render in lists and in the
// context index.
std::string OneLine(std::string_view text) {
    std::string out;
    bool space = false;
    for (const char c : text) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::optional<std::string> OptionalString(const nlohmann::json& input, const char* key) {
    if (auto it = input.find(key); it != input.end() && it->is_string()) return it->get<std::string>();
    return std::nullopt;
}

// Throws with a message the model can act on.
std::string CheckedTitle(const std::string& raw) {
    std::string title = OneLine(raw);
    if (title.empty()) throw std::runtime_error("title is empty");
    if (common::CountCodePoints(title) > kMaxTitleChars) {
        throw std::runtime_error("title is longer than " + std::to_string(kMaxTitleChars) +
                                 " characters -- keep it a short name; detail belongs in the body");
    }
    return title;
}

std::string CheckedDescription(const std::string& raw) {
    std::string description = OneLine(raw);
    if (description.empty()) {
        throw std::runtime_error("description is empty -- one line saying what this entry is about");
    }
    if (common::CountCodePoints(description) > kMaxDescriptionChars) {
        throw std::runtime_error("description is longer than " +
                                 std::to_string(kMaxDescriptionChars) +
                                 " characters -- one line; detail belongs in the body");
    }
    return description;
}

std::string CheckedBody(const MemorySettings& settings, const std::string& body) {
    if (body.empty()) throw std::runtime_error("body is empty");
    if (body.size() > static_cast<std::size_t>(settings.max_text_bytes)) {
        throw std::runtime_error("body exceeds max_text_bytes (" +
                                 std::to_string(settings.max_text_bytes) +
                                 " bytes) -- split it, or write a markdown file and store it by "
                                 "path (each heading section becomes an entry)");
    }
    return body;
}

ScopeFilter ExactScope(int scope, const std::string& project_root) {
    ScopeFilter filter;
    filter.include_global = scope == kScopeGlobal;
    filter.include_project = scope == kScopeProject;
    filter.project_root = project_root;
    return filter;
}

tools::ToolCategory WriteCategory(const MemorySettings& settings) {
    return settings.writes_prompt ? tools::ToolCategory::FileWrite : tools::ToolCategory::ReadOnly;
}

class MemoryStoreTool final : public tools::Tool {
public:
    explicit MemoryStoreTool(std::shared_ptr<MemoryRuntime> runtime) : runtime_(std::move(runtime)) {
        const bool inline_ok = runtime_->Settings()->allow_inline;
        spec_.name = "memory_store";
        spec_.category = WriteCategory(*runtime_->Settings());
        spec_.definition.name = spec_.name;
        // Only what the model cannot infer: what is worth saving, that the
        // description drives retrieval, the cost of pinning, the replace-on-
        // re-store file loop, and update-over-duplicate. Length limits and
        // file-import rules are left to the error messages, which fire exactly
        // when they apply.
        spec_.definition.description =
            std::string("Save something worth knowing in future sessions: the durable point, not "
                        "a transcript. `description` is one line on what it is about; retrieval "
                        "keys on it. `pinned` puts the entry in every session's context, so use "
                        "it only for standing facts or preferences. ") +
            (inline_ok ? "Give `body`, or `path` to import a markdown file (one entry per "
                         "section; storing the same path again replaces them). "
                       : "Write the memory as a markdown file and store it by `path` (one entry "
                         "per section; storing the same path again replaces them). ") +
            "If a similar entry exists, prefer memory_update.";
        nlohmann::json properties = {
            {"title", {{"type", "string"}}},
            {"description", {{"type", "string"}}},
            {"path", {{"type", "string"}}},
            {"pinned", {{"type", "boolean"}}},
            {"scope",
             {{"type", "string"},
              {"enum", {"project", "global"}},
              // The one line the model decides scope from: without a rule and
              // a default, "everywhere" read as the safe choice and project
              // facts leaked into every other project's index.
              {"description",
               "project: anything about or depending on the current project -- its "
               "content, conventions, commands, decisions, people, state of work. "
               "global: only what holds wherever the user works -- their own "
               "preferences, facts about them, know-how not tied to any one project. "
               "If it names or depends on this project, it is project. When unsure, "
               "project."}}}};
        if (inline_ok) properties["body"] = {{"type", "string"}};
        spec_.definition.input_schema = {
            {"type", "object"}, {"properties", properties}, {"required", {"scope"}}};
    }

    const tools::ToolSpec& spec() const override { return spec_; }
    bool path_confinement_supported() const override { return true; }
    std::optional<std::string> ExtractPath(const nlohmann::json& input) const override {
        return OptionalString(input, "path");
    }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        auto& rt = *runtime_;
        StartWorkers(rt);
        try {
            const std::string scope_name = input.value("scope", "");
            if (scope_name != "project" && scope_name != "global") {
                return Error("scope must be \"project\" or \"global\"");
            }
            const int scope = scope_name == "global" ? kScopeGlobal : kScopeProject;
            const std::string root =
                scope == kScopeGlobal ? std::string() : WorktreeRoot(ctx.project_root).string();
            if (scope == kScopeProject && root.empty()) {
                return Error("no project is bound to this session; use scope \"global\"");
            }
            // "text" was v1's name for the body; accepted so a model primed on
            // the old schema is not bounced.
            auto body = OptionalString(input, "body");
            if (!body) body = OptionalString(input, "text");
            const auto path = OptionalString(input, "path");
            if (body.has_value() == path.has_value()) {
                return Error("memory_store needs exactly one of \"body\" or \"path\"");
            }
            const bool pinned = input.value("pinned", false);
            if (path) return StorePath(ctx, scope, root, *path, pinned, input);
            if (!rt.Settings()->allow_inline) {
                return Error("inline bodies are disabled (allow_inline=false): write the memory "
                             "as a markdown file with the file tools, then memory_store it by "
                             "\"path\"");
            }
            return StoreInline(ctx, scope, root, *body, pinned, input);
        } catch (const std::exception& error) {
            return Error(std::string("memory_store: ") + error.what());
        }
    }

private:
    tools::ToolResult StoreInline(tools::ToolContext& ctx, int scope, const std::string& root,
                                  const std::string& body, bool pinned, const nlohmann::json& input) {
        auto& rt = *runtime_;
        NewEntry entry;
        entry.scope = scope;
        entry.project_root = root;
        entry.session_id = ctx.session_id;
        entry.origin = "model";
        entry.title = CheckedTitle(input.value("title", ""));
        entry.description = CheckedDescription(input.value("description", ""));
        entry.body = CheckedBody(*rt.Settings(), body);
        entry.pinned = pinned;
        const std::string id = rt.store->Insert(entry);

        nlohmann::json response{{"id", id}, {"scope", detail::ScopeName(scope)}};
        if (auto similar = SimilarTo(id, entry)) response["similar"] = *similar;
        rt.embeddings->Notify();
        rt.tagger->Notify();
        if (rt.tagger->enabled()) response["tags"] = "pending (filed automatically)";
        return Ok(response);
    }

    // Embeds the new entry right away when the backend is up (so it is
    // immediately findable semantically) and reports a near-duplicate. With
    // no vector, an identical title in the same scope is the duplicate
    // signal.
    std::optional<nlohmann::json> SimilarTo(const std::string& id, const NewEntry& entry) {
        auto& rt = *runtime_;
        const ScopeFilter same_scope = ExactScope(entry.scope, entry.project_root);
        std::optional<std::string> match_id;
        std::optional<double> match_score;
        if (auto vector = rt.embeddings->EmbedNow(EmbedText(entry.title, entry.description, entry.body))) {
            rt.store->StoreEmbedding(id, *vector, rt.embeddings->signature());
            for (const auto& hit : rt.store->Nearest(vector->values, rt.embeddings->signature(),
                                                     same_scope, 2)) {
                if (hit.id != id && hit.score >= kDuplicateSimilarity) {
                    match_id = hit.id;
                    match_score = hit.score;
                    break;
                }
            }
        } else {
            MemoryStore::Filters filters;
            filters.scope = same_scope;
            for (const auto& hit : rt.store->Lexical(entry.title, filters, 5)) {
                if (hit.id == id) continue;
                const auto row = rt.store->Get(hit.id, same_scope);
                if (row && OneLine(row->title) == entry.title) {
                    match_id = hit.id;
                    break;
                }
            }
        }
        if (!match_id) return std::nullopt;
        const auto row = rt.store->Get(*match_id, same_scope);
        if (!row) return std::nullopt;
        nlohmann::json similar{{"id", row->id}, {"title", row->title},
                               {"description", row->description}};
        if (match_score) similar["score"] = detail::RoundScore(*match_score);
        similar["hint"] = "a similar entry already exists: if this one supersedes it, "
                          "memory_update that entry instead and memory_delete this one";
        return similar;
    }

    tools::ToolResult StorePath(tools::ToolContext& ctx, int scope, const std::string& root,
                                const std::string& path, bool pinned, const nlohmann::json& input) {
        auto& rt = *runtime_;
        // Second, safe-resolution pass (the tools-fs pattern): the executor
        // already classified the path for permissions; this resolves it to
        // actually read it.
        const auto resolved = tools::confine(ctx.project_root, path, /*must_exist=*/true);
        if (!resolved.ok) return Error("memory_store: cannot resolve path: " + resolved.error);
        std::error_code size_error;
        const auto size = std::filesystem::file_size(resolved.absolute_path, size_error);
        if (size_error) return Error("memory_store: cannot read " + path + ": " + size_error.message());
        if (size > kMaxIngestFileBytes) {
            return Error("memory_store: " + path + " is " + std::to_string(size) +
                         " bytes; memory files are capped at " +
                         std::to_string(kMaxIngestFileBytes) + " -- store the lessons, not the corpus");
        }
        std::ifstream file(resolved.absolute_path, std::ios::binary);
        if (!file) return Error("memory_store: cannot read " + path);
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (std::string_view(content).substr(0, tools::kBinarySniffBytes).find('\0') !=
            std::string_view::npos) {
            return Error("memory_store: refusing a binary file (memory is text)");
        }

        std::string fallback_title = resolved.absolute_path.stem().string();
        if (auto title = OptionalString(input, "title"); title && !OneLine(*title).empty()) {
            fallback_title = CheckedTitle(*title);
        }
        std::string fallback_description;
        if (auto description = OptionalString(input, "description");
            description && !OneLine(*description).empty()) {
            fallback_description = CheckedDescription(*description);
        }
        const auto chunks = ChunkMarkdown(content, fallback_title, fallback_description,
                                          static_cast<std::size_t>(rt.Settings()->max_text_bytes));
        std::vector<NewEntry> entries;
        for (const auto& chunk : chunks) {
            NewEntry entry;
            entry.scope = scope;
            entry.project_root = root;
            entry.session_id = ctx.session_id;
            entry.title = chunk.title;
            entry.description = chunk.description;
            entry.body = chunk.body;
            entry.pinned = pinned;
            entries.push_back(std::move(entry));
        }
        const auto result = rt.store->Ingest(entries, resolved.absolute_path.string());
        rt.embeddings->Notify();
        rt.tagger->Notify();

        nlohmann::json titles = nlohmann::json::array();
        for (const auto& chunk : chunks) titles.push_back(chunk.title);
        nlohmann::json response{{"group_id", result.group_id},
                                {"source", resolved.absolute_path.string()},
                                {"scope", detail::ScopeName(scope)},
                                {"entries", titles}};
        if (result.replaced > 0) response["replaced"] = result.replaced;
        return Ok(response);
    }

    std::shared_ptr<MemoryRuntime> runtime_;
    tools::ToolSpec spec_;
};

class MemoryUpdateTool final : public tools::Tool {
public:
    explicit MemoryUpdateTool(std::shared_ptr<MemoryRuntime> runtime) : runtime_(std::move(runtime)) {
        spec_.name = "memory_update";
        spec_.category = WriteCategory(*runtime_->Settings());
        spec_.definition.name = spec_.name;
        spec_.definition.description = "Edit a memory entry in place. Pass only the fields to change.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"title", {{"type", "string"}}},
              {"description", {{"type", "string"}}},
              {"body", {{"type", "string"}}},
              {"pinned", {{"type", "boolean"}}}}},
            {"required", {"id"}}};
    }

    const tools::ToolSpec& spec() const override { return spec_; }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        auto& rt = *runtime_;
        StartWorkers(rt);
        try {
            const std::string id = input.value("id", "");
            if (id.empty()) return Error("memory_update needs \"id\"");
            EntryPatch patch;
            if (auto title = OptionalString(input, "title")) patch.title = CheckedTitle(*title);
            if (auto description = OptionalString(input, "description")) {
                patch.description = CheckedDescription(*description);
            }
            if (auto body = OptionalString(input, "body")) patch.body = CheckedBody(*rt.Settings(), *body);
            if (auto it = input.find("pinned"); it != input.end() && it->is_boolean()) {
                patch.pinned = it->get<bool>();
            }
            if (!patch.title && !patch.description && !patch.body && !patch.pinned) {
                return Error("nothing to update: pass title, description, body or pinned");
            }
            const ScopeFilter visible = VisibleFrom(ctx.project_root);
            switch (rt.store->Update(id, patch, visible)) {
                case WriteOutcome::kNotFound:
                    return Error("no such memory entry: " + id);
                case WriteOutcome::kIngestChunk: {
                    const auto row = rt.store->Get(id, visible);
                    return Error("this entry was imported from " +
                                 (row ? row->origin_ref : std::string("a file")) +
                                 ": edit the file and memory_store it again by path");
                }
                case WriteOutcome::kDone:
                    break;
            }
            if (patch.title || patch.description || patch.body) {
                rt.embeddings->Notify();
                rt.tagger->Notify();
            }
            return Ok({{"id", id}, {"updated", true}});
        } catch (const std::exception& error) {
            return Error(std::string("memory_update: ") + error.what());
        }
    }

private:
    std::shared_ptr<MemoryRuntime> runtime_;
    tools::ToolSpec spec_;
};

class MemoryDeleteTool final : public tools::Tool {
public:
    explicit MemoryDeleteTool(std::shared_ptr<MemoryRuntime> runtime) : runtime_(std::move(runtime)) {
        spec_.name = "memory_delete";
        spec_.category = WriteCategory(*runtime_->Settings());
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Delete a memory entry by `id`, or a whole file import by `group_id`.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties", {{"id", {{"type", "string"}}}, {"group_id", {{"type", "string"}}}}}};
    }

    const tools::ToolSpec& spec() const override { return spec_; }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        auto& rt = *runtime_;
        StartWorkers(rt);
        try {
            const ScopeFilter visible = VisibleFrom(ctx.project_root);
            const std::string id = input.value("id", "");
            const std::string group = input.value("group_id", "");
            if (id.empty() == group.empty()) {
                return Error("memory_delete needs exactly one of \"id\" or \"group_id\"");
            }
            if (!group.empty()) {
                const int removed = rt.store->DeleteGroup(group, visible);
                if (removed == 0) return Error("no such group: " + group);
                return Ok({{"deleted", removed}});
            }
            switch (rt.store->Delete(id, visible)) {
                case WriteOutcome::kNotFound:
                    return Error("no such memory entry: " + id);
                case WriteOutcome::kIngestChunk: {
                    const auto row = rt.store->Get(id, visible);
                    return Error("this entry was imported from " +
                                 (row ? row->origin_ref : std::string("a file")) +
                                 " and would come back on the next import: edit the file and "
                                 "store it again, or delete the whole import with group_id \"" +
                                 (row && row->group_id ? *row->group_id : std::string()) + "\"");
                }
                case WriteOutcome::kDone:
                    break;
            }
            return Ok({{"deleted", 1}});
        } catch (const std::exception& error) {
            return Error(std::string("memory_delete: ") + error.what());
        }
    }

private:
    std::shared_ptr<MemoryRuntime> runtime_;
    tools::ToolSpec spec_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakeMemoryStoreTool(std::shared_ptr<MemoryRuntime> runtime) {
    return std::make_unique<MemoryStoreTool>(std::move(runtime));
}
std::unique_ptr<tools::Tool> MakeMemoryUpdateTool(std::shared_ptr<MemoryRuntime> runtime) {
    return std::make_unique<MemoryUpdateTool>(std::move(runtime));
}
std::unique_ptr<tools::Tool> MakeMemoryDeleteTool(std::shared_ptr<MemoryRuntime> runtime) {
    return std::make_unique<MemoryDeleteTool>(std::move(runtime));
}

}  // namespace minimal::memory
