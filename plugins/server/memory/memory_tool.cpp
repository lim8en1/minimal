#include "memory_tool.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "minimal/agent/session_support_service.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/plugins/service_registry.hpp"
#include "minimal/tools/path_confinement.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::memory {

// Worktree-root walk: the same convention presets and skill lookup use
// (docs/CONFIGURATION.md "Project-local skills") -- walk up from the session
// root to the enclosing worktree root, marked by .git (directory in the
// normal case, a FILE in a linked worktree). The memory plugin resolves
// scope=project identically so scope is stable across subdirectories.
// Declared in memory_tool.hpp: MemoryCommand (/memory) needs the same
// resolution to report project-scope topics, not just the tools.
std::filesystem::path WorktreeRoot(const std::filesystem::path& project_root) {
    std::filesystem::path current = project_root;
    const std::filesystem::path fallback = project_root;
    while (!current.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(current / ".git", ec)) return current;
        auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return fallback;
}

namespace {

constexpr int kHardLimitCeiling = 64;
constexpr double kSimilarHintThreshold = 0.92;

// 128 code points, not bytes (session-titler's rule -- a byte cut would
// render U+FFFD in the surface the model reads).
std::string PreviewOf(std::string_view text, std::size_t max_chars) {
    std::size_t chars = 0, i = 0;
    while (i < text.size() && chars < max_chars) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2
                          : (c & 0xF0) == 0xE0 ? 3
                          : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (i + len > text.size()) len = text.size() - i;
        i += len;
        ++chars;
    }
    return std::string(text.substr(0, i));
}

// One scope name -> (scope, project_root). scope=project walks to the
// worktree root; scope=global roots at ''.
struct ScopeResolved {
    int scope;
    std::string project_root;
    std::string scope_name;
};

ScopeResolved ResolveScope(tools::ToolContext& ctx, const std::string& requested) {
    if (requested == "global") return {MemoryStore::kScopeGlobal, "", "global"};
    if (requested == "project" || requested.empty())
        return {MemoryStore::kScopeProject, WorktreeRoot(ctx.project_root).string(), "project"};
    throw std::runtime_error("scope must be \"project\" or \"global\"");
}

// Lazy re-embed drain: budgeted (reindex_budget_rows), synchronous, at the
// top of each memory tool call. With the sidecar backend this is one round
// trip to the already-running subprocess; with the http backend it is one
// batched request. Deliberately no background thread: nothing to
// lifetime-manage at process teardown, and the budget bounds the latency
// every caller already accepts by invoking a tool.
void MaybeDrainReindex(MemoryRuntime& rt) {
    if (rt.embedder == nullptr) return;
    auto pending = rt.store->ClaimPendingEmbeddings(rt.signature, rt.settings.reindex_budget_rows);
    if (pending.empty()) return;
    std::vector<std::string> texts;
    texts.reserve(pending.size());
    for (const auto& row : pending) texts.push_back(row.text);
    std::size_t stored = 0;  // how far the per-row store loop got
    try {
        std::vector<Embedding> embeddings = rt.embedder->Embed(texts);
        if (embeddings.size() != pending.size()) {
            throw std::runtime_error("backend returned " + std::to_string(embeddings.size()) +
                                     " embeddings for " + std::to_string(pending.size()) +
                                     " inputs");
        }
        for (; stored < pending.size(); ++stored) {
            rt.store->StoreEmbedding(pending[stored].id, embeddings[stored], rt.signature);
        }
    } catch (const std::exception& error) {
        // Only the rows NOT yet stored in THIS batch: a row at index <
        // `stored` already has a correct embed_status=1 from the loop above,
        // and marking it failed too would overwrite that real success with a
        // spurious failure just because a LATER row's own store call threw.
        std::vector<std::string> failed_ids;
        for (std::size_t i = stored; i < pending.size(); ++i) failed_ids.push_back(pending[i].id);
        rt.store->MarkEmbedFailed(failed_ids, error.what());
    }
}

// The digest: first memory call of a session only (memoized by session id,
// fresh every session). Empty string = already shown / disabled.
std::string DigestJson(MemoryRuntime& rt, const std::string& session_id,
                       const std::filesystem::path& project_root) {
    if (!rt.settings.digest_enabled) return "";
    {
        std::lock_guard<std::mutex> lock(rt.digest_mu);
        if (session_id.empty() || !rt.digest_shown.insert(session_id).second) return "";
    }
    const Digest digest = rt.store->DigestInfo(
        rt.embedder ? rt.embedder->Describe() : "lexical only",
        rt.enricher->enabled() ? "on (" + rt.settings.summarizer_model + ")" : "off",
        WorktreeRoot(project_root).string());
    nlohmann::json topics = nlohmann::json::array();
    for (const auto& [name, count] : digest.top_topics) {
        topics.push_back({{"topic", name}, {"n", count}});
    }
    nlohmann::json j;
    j["memory_digest"] = {{"entries", digest.entries_by_scope},
                          {"by_origin", digest.by_origin},
                          {"topics", topics},
                          {"backend", digest.backend},
                          {"enrichment", digest.enrichment},
                          {"pending_reindex", digest.pending_reindex}};
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// Result body with the digest riding on the first memory call of a session.
std::string WithDigest(MemoryRuntime& rt, tools::ToolContext& ctx, std::string body) {
    std::string digest = DigestJson(rt, ctx.session_id, ctx.project_root);
    if (digest.empty()) return body;
    return digest + "\n" + body;
}

nlohmann::json HitJson(const SearchHit& hit) {
    nlohmann::json j;
    j["id"] = hit.id;
    j["preview"] = hit.preview;
    if (hit.summary) j["summary"] = *hit.summary;
    if (hit.topic) j["topic"] = *hit.topic;
    j["created_at"] = hit.created_at;
    if (hit.score > 0.0) j["score"] = hit.score;
    return j;
}

// ---------------------------------------------------------------- search --

class MemorySearchTool final : public tools::Tool {
public:
    explicit MemorySearchTool(std::shared_ptr<MemoryRuntime> runtime)
        : runtime_(std::move(runtime)) {}

    const tools::ToolSpec& spec() const override {
        static const tools::ToolSpec spec = {
            "memory_search",
            tools::ToolCategory::ReadOnly,
            {"memory_search",
             "Search durable memory (lessons learned in past sessions). Use BEFORE starting "
             "work in an area not touched this session, before re-deriving a workflow or "
             "convention the user has described before, and on failures that smell like a "
             "known quirk. Call with query=null to browse what is stored; filter by topic "
             "when the digest showed a relevant one; prefer memory_get for the full text of "
             "a hit. Results are ranked by similarity when embeddings are available, else "
             "by recency. Store new lessons with memory_store; supersede stale ones with "
             "delete+store.",
             {{"type", "object"},
              {"properties",
               {{"query",
                 {{"type", "string"},
                  {"description", "Free-text semantic query. Omit to browse by recency."}}},
                {"keywords",
                 {{"type", "array", "items", {{"type", "string"}}},
                  {"description",
                   "AND-ed substring prefilters over text, origin file path and topic names "
                   "(e.g. [\"docker\"]) -- precise, works without embeddings."}}},
                {"topic", {{"type", "string"}, {"description", "Filter to one topic name."}}},
                {"scope",
                 {{"type", "string", "enum", {"project", "global"}},
                  {"description", "project = lessons for this worktree; global = machine-wide."}}},
                {"sort", {{"type", "string", "enum", {"sim", "time"}}}},
                {"limit", {{"type", "integer", "minimum", 1, "maximum", 64}}}}},
              {"required", {"scope"}}}}};
        return spec;
    }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        auto& rt = *runtime_;
        tools::ToolResult result;
        try {
            MaybeDrainReindex(rt);
            const ScopeResolved scope = ResolveScope(ctx, input.value("scope", ""));
            MemoryStore::SearchQuery query;
            query.scope = scope.scope;
            query.project_root = scope.project_root;
            if (auto it = input.find("query"); it != input.end() && it->is_string() &&
                                                 !it->get<std::string>().empty()) {
                query.query = it->get<std::string>();
            }
            if (auto it = input.find("keywords"); it != input.end() && it->is_array()) {
                for (const auto& keyword : *it) {
                    if (keyword.is_string() && !keyword.get<std::string>().empty()) {
                        query.keywords.push_back(keyword.get<std::string>());
                    }
                }
            }
            if (auto it = input.find("topic"); it != input.end() && it->is_string()) {
                query.topic = it->get<std::string>();
            }
            query.sort = input.value("sort", rt.embedder ? "sim" : "time");
            if (query.sort != "sim" && query.sort != "time") query.sort = "time";
            query.limit = input.value("limit", rt.settings.default_limit);
            if (query.limit < 1) query.limit = 1;
            if (query.limit > kHardLimitCeiling) query.limit = kHardLimitCeiling;
            query.signature = rt.signature;
            query.has_embeddings = rt.embedder != nullptr;

            // Semantic pass: embed the query once, score stored vectors.
            std::vector<SearchHit> hits = rt.store->Search(query);
            if (query.sort == "sim" && rt.embedder != nullptr && query.query) {
                try {
                    const Embedding q = rt.embedder->Embed({*query.query}).at(0);
                    const auto vectors = rt.store->LoadVectors(rt.signature, query.scope,
                                                               query.project_root);
                    std::vector<std::pair<double, SearchHit>> scored;
                    for (auto& hit : hits) {
                        double best = 0.0;
                        for (const auto& row : vectors) {
                            if (row.id == hit.id) {
                                best = Cosine(q.values, row.values);
                                break;
                            }
                        }
                        scored.push_back({best, std::move(hit)});
                    }
                    std::stable_sort(scored.begin(), scored.end(),
                                     [](const auto& a, const auto& b) { return a.first > b.first; });
                    hits.clear();
                    for (auto& [score, hit] : scored) {
                        hit.score = score;
                        if (static_cast<int>(hits.size()) < query.limit) hits.push_back(std::move(hit));
                    }
                } catch (const std::exception&) {
                    // Embedding the query failed: fall through with the
                    // lexical candidates, recency order, score absent.
                    // `hits` here is still Search()'s full-scan result (no
                    // FTS gate, LIMIT INT_MAX -- the whole point of a sim
                    // sort is to rerank before truncating), so it must be
                    // capped here the same way the success path caps it
                    // above, or the fallback returns every row in scope.
                    if (static_cast<int>(hits.size()) > query.limit) {
                        hits.resize(static_cast<std::size_t>(std::max(query.limit, 0)));
                    }
                }
            }

            nlohmann::json results = nlohmann::json::array();
            for (const auto& hit : hits) results.push_back(HitJson(hit));
            nlohmann::json body;
            body["results"] = results;
            if (results.empty()) body["note"] = "no matches";
            result.content_text = WithDigest(rt, ctx, body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        } catch (const std::exception& error) {
            result.is_error = true;
            result.content_text = error.what();
        }
        return result;
    }

private:
    std::shared_ptr<MemoryRuntime> runtime_;
};

// ----------------------------------------------------------------- store --

// Markdown heading split for the path form (^#{1,6} ). Chunks are
// self-describing: the heading travels with its body.
std::vector<std::string> ChunkOnHeadings(const std::string& text) {
    std::vector<std::string> chunks;
    std::string current;
    for (std::size_t line_start = 0; line_start <= text.size();) {
        const auto line_end = text.find('\n', line_start);
        const std::string_view line = std::string_view(text)
                                          .substr(line_start,
                                                  line_end == std::string_view::npos
                                                      ? std::string_view::npos
                                                      : line_end - line_start);
        const bool heading = !line.empty() && line[0] == '#' &&
                             line.find_first_not_of('#') != std::string_view::npos &&
                             line[line.find_first_not_of('#')] == ' ';
        if (heading && !current.empty()) {
            chunks.push_back(current);
            current.clear();
        }
        current.append(line).append("\n");
        if (line_end == std::string::npos) break;
        line_start = line_end + 1;
    }
    if (!current.empty()) chunks.push_back(current);
    // Split oversized chunks on blank lines (max_text_bytes still enforced
    // per row below -- a chunk that never fits is refused, not truncated).
    return chunks;
}

class MemoryStoreTool final : public tools::Tool {
public:
    explicit MemoryStoreTool(std::shared_ptr<MemoryRuntime> runtime)
        : runtime_(std::move(runtime)),
          spec_{"memory_store", tools::ToolCategory::ReadOnly,
                {"memory_store", "",
                 {{"type", "object"},
                  {"properties",
                   {{"text", {{"type", "string"}, {"description", "The lesson, inline."}}},
                    {"path", {{"type", "string"}, {"description", "Markdown file to ingest."}}},
                    {"scope",
                     {{"type", "string", "enum", {"project", "global"}},
                      {"description", "project = for this worktree; global = machine-wide."}}}}},
                  {"required", {"scope"}}}}} {}

    const tools::ToolSpec& spec() const override {
        // Description composed per mode (settings parse at registration):
        // under allow_inline=false the inline mention is dropped and the
        // drafting loop is taught instead. Built once, on first call.
        if (spec_.definition.description.empty()) {
            spec_.definition.description =
                runtime_->settings.allow_inline
                    ? "Store a durable lesson for future sessions. Exactly one of text/path. "
                      "text: a short, self-contained lesson (a workflow, a gotcha, a user "
                      "preference) captured inline. path: a markdown file to import as "
                      "memory -- for anything long or composite, draft it with the file "
                      "tools and store by path; re-ingesting the same path REPLACES its "
                      "previous entries, which is how you refine a memory. Scope: project "
                      "lessons (this worktree) or global (machine-wide, e.g. user "
                      "preferences). Before storing, consider memory_search: a similar "
                      "entry may exist -- delete+store to supersede it."
                    : "Store a durable lesson for future sessions. Memories are authored as "
                      "markdown files with the file tools, then stored by path (the inline "
                      "text option is disabled in this configuration). Re-ingesting the "
                      "same path REPLACES its previous entries -- edit the file and store "
                      "again to refine a memory. Scope: project (this worktree) or global "
                      "(machine-wide). Before storing, consider memory_search: a similar "
                      "entry may exist -- delete+store to supersede it.";
        }
        return spec_;
    }

    bool path_confinement_supported() const override { return true; }

    std::optional<std::string> ExtractPath(const nlohmann::json& input) const override {
        if (auto it = input.find("path"); it != input.end() && it->is_string()) {
            return it->get<std::string>();
        }
        return std::nullopt;
    }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        auto& rt = *runtime_;
        tools::ToolResult result;
        try {
            MaybeDrainReindex(rt);
            const ScopeResolved scope = ResolveScope(ctx, input.value("scope", ""));
            const bool has_text = input.contains("text");
            const bool has_path = input.contains("path");
            if (has_text == has_path) {
                throw std::runtime_error(
                    "memory_store needs exactly one of \"text\" or \"path\"");
            }

            // Resolve the summarizer model ref NOW, on the tool thread, while
            // the session exists (titler's rule); the detached enrichment
            // workers only use the resolved handle.
            ResolveEnricherLlm(rt, ctx);

            if (has_text) {
                if (!rt.settings.allow_inline) {
                    throw std::runtime_error(
                        "inline text storage is disabled (allow_inline=false): draft the "
                        "lesson as a markdown file with the file tools, then memory_store "
                        "with \"path\"");
                }
                result = StoreText(rt, ctx, scope, input.at("text").get<std::string>());
            } else {
                result = StorePath(rt, ctx, scope, input.at("path").get<std::string>());
            }
            result.content_text = WithDigest(rt, ctx, result.content_text);
        } catch (const std::exception& error) {
            result.is_error = true;
            result.content_text = error.what();
        }
        return result;
    }

private:
    std::shared_ptr<MemoryRuntime> runtime_;
    // mutable: the description is composed on first spec() (mode-dependent)
    mutable tools::ToolSpec spec_;

    static void ResolveEnricherLlm(MemoryRuntime& rt, tools::ToolContext& ctx) {
        if (rt.enricher == nullptr || !rt.enricher->enabled()) return;
        if (rt.services == nullptr) return;
        void* raw = rt.services->Find(agent::kSessionSupportServiceName);
        if (raw == nullptr) return;
        auto* support = static_cast<agent::SessionSupportService*>(raw);
        if (auto llm = support->ResolveModelRef(rt.settings.summarizer_model)) {
            rt.enricher->SetLlm(llm->provider, llm->model);
        }
    }

    // Eager embedding + the near-duplicate hint. Failures degrade silently:
    // the row stays pending for the next drain.
    std::optional<std::pair<std::string, double>> EmbedAndHint(MemoryRuntime& rt,
                                                               const ScopeResolved& scope,
                                                               const std::string& id,
                                                               const std::string& text) {
        if (rt.embedder == nullptr) return std::nullopt;
        try {
            const Embedding embedding = rt.embedder->Embed({text}).at(0);
            const auto vectors =
                rt.store->LoadVectors(rt.signature, scope.scope, scope.project_root);
            std::optional<std::pair<std::string, double>> hint;
            for (const auto& row : vectors) {
                const double score = Cosine(embedding.values, row.values);
                if (score > kSimilarHintThreshold &&
                    (!hint || score > hint->second)) {
                    hint = {row.id, score};
                }
            }
            rt.store->StoreEmbedding(id, embedding, rt.signature);
            return hint;
        } catch (const std::exception&) {
            return std::nullopt;  // row stays embed_status=0; next drain retries
        }
    }

    tools::ToolResult StoreText(MemoryRuntime& rt, tools::ToolContext& ctx,
                                const ScopeResolved& scope, const std::string& text) {
        if (text.empty()) throw std::runtime_error("memory_store: text is empty");
        if (text.size() > static_cast<std::size_t>(rt.settings.max_text_bytes)) {
            throw std::runtime_error("memory_store: text exceeds max_text_bytes (" +
                                     std::to_string(rt.settings.max_text_bytes) +
                                     " bytes) -- draft a file and store by path");
        }
        NewEntry entry;
        entry.scope = scope.scope;
        entry.project_root = scope.project_root;
        entry.session_id = ctx.session_id;
        entry.origin = "model";
        entry.text = text;
        const std::string id = rt.store->Insert(entry);

        tools::ToolResult result;
        nlohmann::json body;
        body["id"] = id;
        body["preview"] = PreviewOf(text, 128);
        body["summary_pending"] = rt.enricher->enabled();
        if (auto hint = EmbedAndHint(rt, scope, id, text)) {
            body["similar"] = {{"id", hint->first}, {"score", hint->second},
                               {"hint", "a near-duplicate entry exists -- memory_delete it "
                                        "and store again if this supersedes it"}};
        }
        rt.enricher->Enqueue(id, text, scope.scope, scope.project_root);
        result.content_text = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        return result;
    }

    tools::ToolResult StorePath(MemoryRuntime& rt, tools::ToolContext& ctx,
                                const ScopeResolved& scope, const std::string& path) {
        // Second, safe-resolution pass (tools-fs pattern): the executor
        // already classified the path for permissions; we re-resolve to
        // actually read it.
        const auto resolved = tools::confine(ctx.project_root, path, /*must_exist=*/false);
        if (!resolved.ok) throw std::runtime_error("memory_store: cannot resolve path: " + resolved.error);
        std::ifstream file(resolved.absolute_path, std::ios::binary);
        if (!file) throw std::runtime_error("memory_store: cannot read " + path);
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        // Binary refusal: a NUL in the first 8 KiB is binary for read_window's
        // purposes too; markdown memory is text by construction. string_view
        // scans in place -- no need to copy the first 8 KiB just to search it.
        if (std::string_view(content).substr(0, 8192).find('\0') != std::string_view::npos) {
            throw std::runtime_error("memory_store: refusing binary file (memory is text)");
        }
        if (content.empty()) throw std::runtime_error("memory_store: file is empty");

        const auto chunks = ChunkOnHeadings(content);
        const std::string origin_ref = resolved.absolute_path.string();
        for (const auto& chunk : chunks) {
            if (chunk.size() > static_cast<std::size_t>(rt.settings.max_text_bytes)) {
                throw std::runtime_error(
                    "memory_store: a heading section exceeds max_text_bytes (" +
                    std::to_string(rt.settings.max_text_bytes) + " bytes) -- split it");
            }
        }

        NewEntry first;
        first.scope = scope.scope;
        first.project_root = scope.project_root;
        first.session_id = ctx.session_id;
        first.origin = "ingest";
        first.text = chunks[0];
        std::vector<NewEntry> rest;
        for (std::size_t i = 1; i < chunks.size(); ++i) {
            NewEntry chunk = first;
            chunk.text = chunks[i];
            rest.push_back(std::move(chunk));
        }
        const auto ingest = rt.store->Ingest(first, rest, origin_ref);

        // One enrichment call per STORE OPERATION, not per chunk: sample =
        // first chunk (headings travel with their body, so it carries the
        // structure); its topic applies to the whole group, so every other
        // chunk's id rides along as also_bind and gets the same
        // summary/topic without a provider call each.
        const std::vector<std::string> also_bind(ingest.ids.begin() + 1, ingest.ids.end());
        rt.enricher->Enqueue(ingest.ids.front(), chunks[0], scope.scope, scope.project_root,
                             also_bind);

        tools::ToolResult result;
        nlohmann::json body;
        body["group_id"] = ingest.group_id;
        body["chunks"] = ingest.ids.size();
        if (ingest.replaced > 0) body["replaced"] = ingest.replaced;
        body["preview"] = PreviewOf(chunks[0], 128);
        body["summary_pending"] = rt.enricher->enabled();
        result.content_text = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        return result;
    }
};

// -------------------------------------------------------------------- get --

class MemoryGetTool final : public tools::Tool {
public:
    explicit MemoryGetTool(std::shared_ptr<MemoryRuntime> runtime)
        : runtime_(std::move(runtime)) {}

    const tools::ToolSpec& spec() const override {
        static const tools::ToolSpec spec = {
            "memory_get",
            tools::ToolCategory::ReadOnly,
            {"memory_get", "Fetch the full text of one memory entry by id (search results "
                           "carry previews; this returns everything, including the source "
                           "file and sibling position for ingested chunks).",
             {{"type", "object"},
              {"properties", {{"id", {{"type", "string"}}}}},
              {"required", {"id"}}}}};
        return spec;
    }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        auto& rt = *runtime_;
        tools::ToolResult result;
        try {
            MaybeDrainReindex(rt);
            const auto entry = rt.store->Get(input.at("id").get<std::string>());
            if (!entry) {
                result.is_error = true;
                result.content_text = "no such memory entry";
                return result;
            }
            nlohmann::json j;
            j["id"] = entry->id;
            j["text"] = entry->text;
            if (entry->summary) j["summary"] = *entry->summary;
            if (entry->topic) j["topic"] = *entry->topic;
            j["scope"] = entry->scope_name;
            if (!entry->project_root.empty()) j["project_root"] = entry->project_root;
            if (!entry->origin_ref.empty()) j["origin_ref"] = entry->origin_ref;
            j["origin"] = entry->origin;
            if (entry->group_id) {
                j["chunk"] = {{"index", entry->chunk_index},
                              {"count", entry->chunk_count},
                              {"siblings", entry->sibling_ids}};
            }
            j["created_at"] = entry->created_at;
            result.content_text = WithDigest(rt, ctx, j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        } catch (const std::exception& error) {
            result.is_error = true;
            result.content_text = error.what();
        }
        return result;
    }

private:
    std::shared_ptr<MemoryRuntime> runtime_;
};

// ----------------------------------------------------------------- delete --

class MemoryDeleteTool final : public tools::Tool {
public:
    explicit MemoryDeleteTool(std::shared_ptr<MemoryRuntime> runtime)
        : runtime_(std::move(runtime)) {}

    const tools::ToolSpec& spec() const override {
        static const tools::ToolSpec spec = {
            "memory_delete",
            tools::ToolCategory::ReadOnly,
            {"memory_delete", "Delete one memory entry by id. Use to supersede: delete a "
                              "stale entry, then memory_store the replacement.",
             {{"type", "object"},
              {"properties", {{"id", {{"type", "string"}}}}},
              {"required", {"id"}}}}};
        return spec;
    }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext&) override {
        auto& rt = *runtime_;
        tools::ToolResult result;
        try {
            const bool existed = rt.store->Delete(input.at("id").get<std::string>());
            result.content_text = existed ? "deleted" : "no such entry";
        } catch (const std::exception& error) {
            result.is_error = true;
            result.content_text = error.what();
        }
        return result;
    }

private:
    std::shared_ptr<MemoryRuntime> runtime_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakeMemorySearchTool(std::shared_ptr<MemoryRuntime> runtime) {
    return std::make_unique<MemorySearchTool>(std::move(runtime));
}
std::unique_ptr<tools::Tool> MakeMemoryStoreTool(std::shared_ptr<MemoryRuntime> runtime) {
    return std::make_unique<MemoryStoreTool>(std::move(runtime));
}
std::unique_ptr<tools::Tool> MakeMemoryGetTool(std::shared_ptr<MemoryRuntime> runtime) {
    return std::make_unique<MemoryGetTool>(std::move(runtime));
}
std::unique_ptr<tools::Tool> MakeMemoryDeleteTool(std::shared_ptr<MemoryRuntime> runtime) {
    return std::make_unique<MemoryDeleteTool>(std::move(runtime));
}

}  // namespace minimal::memory
