#include "tagger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "memory_store.hpp"
#include "search.hpp"

#include "minimal/common/logger.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/llm/stream_event.hpp"

namespace minimal::memory {
namespace {

constexpr int kPendingScan = 32;           // rows looked at per batch
constexpr std::size_t kBatchBytes = 24000;  // entry text per provider call
constexpr int kNeighbors = 5;
constexpr std::chrono::milliseconds kIdlePoll{60000};

std::string ResponseText(const llm::ChatResponse& response) {
    std::string out;
    for (const auto& block : response.content) {
        if (const auto* text = std::get_if<llm::TextBlock>(&block)) out += text->text;
    }
    return out;
}

double Cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double dot = 0, na = 0, nb = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na += static_cast<double>(a[i]) * a[i];
        nb += static_cast<double>(b[i]) * b[i];
    }
    return na == 0 || nb == 0 ? 0.0 : dot / (std::sqrt(na) * std::sqrt(nb));
}

std::string BuildPrompt(const MemorySettings& settings, const std::vector<TagInfo>& vocabulary,
                        bool vocabulary_full, const std::vector<PendingTagRow>& rows,
                        const std::vector<std::vector<std::string>>& neighbor_tags) {
    std::string prompt =
        "You file memory entries into a tag vocabulary. Tags let a reader browse and filter "
        "memories by subject.\n"
        "Rules:\n"
        "- Prefer existing tags. The tags of similar entries are a strong hint.\n"
        "- Propose a new tag only when no existing tag fits the entry's subject. Give it a short "
        "lowercase kebab-case name and a one-line description of what belongs under it.\n"
        "- Never propose a new tag that means the same as an existing one.\n"
        "- At most " + std::to_string(settings.max_tags_per_entry) +
        " tags per entry. Zero tags is fine when nothing fits.\n";
    if (vocabulary_full) {
        prompt += "- The vocabulary is FULL: use existing tags only, never propose new ones.\n";
    }
    prompt += "\nExisting tags (name: description [entries]):\n";
    if (vocabulary.empty()) prompt += "(none yet)\n";
    for (const auto& tag : vocabulary) {
        prompt += "- " + tag.name + ": " + (tag.description.empty() ? "-" : tag.description) +
                  " [" + std::to_string(tag.count) + "]\n";
    }
    prompt += "\nEntries:\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        prompt += "\n[" + std::to_string(i + 1) + "] Title: " + rows[i].title +
                  "\nDescription: " + rows[i].description + "\nBody:\n" + rows[i].body + "\n";
        if (!neighbor_tags[i].empty()) {
            prompt += "Similar entries were tagged:";
            for (const auto& tag : neighbor_tags[i]) prompt += " " + tag;
            prompt += "\n";
        }
    }
    prompt +=
        "\nReply with ONLY a JSON object, no other text:\n"
        "{\"entries\": [{\"n\": 1, \"tags\": [\"existing-tag\"], "
        "\"new\": [{\"name\": \"new-tag\", \"description\": \"what belongs under it\"}]}]}\n"
        "One element per entry, \"n\" being the entry number above.";
    return prompt;
}

}  // namespace

std::optional<std::string> ExtractJsonObjectText(std::string_view text) {
    const auto begin = text.find('{');
    if (begin == std::string_view::npos) return std::nullopt;
    for (std::size_t end = text.size(); end > begin + 1; --end) {
        if (text[end - 1] != '}') continue;
        const auto candidate = text.substr(begin, end - begin);
        if (nlohmann::json::accept(candidate)) return std::string(candidate);
    }
    return std::nullopt;
}

Tagger::Tagger(MemorySettings settings, MemoryStore* store, EmbeddingService* embeddings,
               Resolver resolver, common::Logger& logger)
    : settings_(std::move(settings)),
      store_(store),
      embeddings_(embeddings),
      resolver_(std::move(resolver)),
      logger_(logger) {}

Tagger::~Tagger() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    // Bounded: an in-flight provider call is streamed and its sink aborts on
    // stop_, and every call carries tagger_timeout_ms besides.
    if (worker_.joinable()) worker_.join();
}

void Tagger::Start() {
    if (!enabled() || worker_.joinable()) return;
    worker_ = std::thread([this] { WorkerLoop(); });
}

void Tagger::Notify() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        notified_ = true;
    }
    cv_.notify_all();
}

std::optional<TaggerLlm> Tagger::Llm() {
    std::lock_guard<std::mutex> lock(llm_mu_);
    if (!llm_ && resolver_) llm_ = resolver_();
    return llm_;
}

std::vector<std::string> Tagger::NeighborTags(const PendingTagRow& row) {
    SearchRequest request;
    request.query = row.title + " " + row.description;
    request.filters.scope.include_global = row.scope == kScopeGlobal;
    request.filters.scope.include_project = row.scope == kScopeProject;
    request.filters.scope.project_root = row.project_root;
    request.limit = kNeighbors + 4;
    std::map<std::string, int> votes;
    int used = 0;
    for (const auto& hit : HybridSearch(*store_, embeddings_, request)) {
        if (hit.row.id == row.id || hit.row.tags.empty()) continue;
        for (const auto& tag : hit.row.tags) ++votes[tag];
        if (++used >= kNeighbors) break;
    }
    std::vector<std::pair<std::string, int>> ranked(votes.begin(), votes.end());
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::string> out;
    for (const auto& [tag, count] : ranked) out.push_back(tag);
    return out;
}

std::optional<std::vector<float>> Tagger::TagVector(std::int64_t tag_id, const std::string& text) {
    if (tag_id != 0) {
        if (auto it = tag_vectors_.find(tag_id); it != tag_vectors_.end()) return it->second;
    }
    if (embeddings_ == nullptr) return std::nullopt;
    auto embedded = embeddings_->EmbedNow(text);
    if (!embedded) return std::nullopt;
    if (tag_id != 0) tag_vectors_[tag_id] = embedded->values;
    return std::move(embedded->values);
}

std::optional<std::int64_t> Tagger::ResolveNewTag(int scope, const std::string& project_root,
                                                  const std::string& name,
                                                  const std::string& description) {
    const std::string normalized = NormalizeTagName(name);
    if (TagMatchKey(normalized).empty()) return std::nullopt;
    // 1. Same match key: it already exists (case, separators, plural).
    if (const auto existing = store_->FindTag(scope, project_root, normalized)) return existing->id;

    // 2. Semantically the same as an existing tag.
    const auto vocabulary = store_->Tags(scope, project_root);
    if (!vocabulary.empty()) {
        if (const auto proposed = TagVector(0, normalized + ": " + description)) {
            std::int64_t best_id = 0;
            double best = 0;
            for (const auto& tag : vocabulary) {
                const auto vector = TagVector(tag.id, tag.name + ": " + tag.description);
                if (!vector) break;  // embedder went away mid-loop: skip the check
                const double score = Cosine(*proposed, *vector);
                if (score > best) {
                    best = score;
                    best_id = tag.id;
                }
            }
            if (best_id != 0 && best >= settings_.tag_merge_similarity) return best_id;
        }
    }

    // 3. The cap.
    if (static_cast<int>(vocabulary.size()) >= settings_.max_tags) return std::nullopt;
    return store_->CreateTag(scope, project_root, normalized, description);
}

bool Tagger::RunOnce() {
    if (!enabled()) return false;
    if (!breaker_.Allow(CircuitBreaker::Clock::now())) return false;
    const auto pending = store_->PendingTagging(kPendingScan);
    if (pending.empty()) return false;

    // One scope per call: the vocabulary is per scope.
    std::vector<PendingTagRow> rows;
    std::size_t bytes = 0;
    for (const auto& row : pending) {
        if (row.scope != pending.front().scope || row.project_root != pending.front().project_root) {
            continue;
        }
        const std::size_t size = row.title.size() + row.description.size() + row.body.size();
        if (!rows.empty() && bytes + size > kBatchBytes) break;
        rows.push_back(row);
        bytes += size;
    }
    const int scope = rows.front().scope;
    const std::string root = rows.front().project_root;

    const auto llm = Llm();
    if (!llm || llm->provider == nullptr) {
        breaker_.RecordFailure(CircuitBreaker::Clock::now(),
                               "cannot resolve tagger_model '" + settings_.tagger_model + "'");
        logger_.warn("memory: tagging paused: cannot resolve tagger_model '{}'",
                     settings_.tagger_model);
        return false;
    }

    const auto vocabulary = store_->Tags(scope, root);
    const bool full = static_cast<int>(vocabulary.size()) >= settings_.max_tags;
    std::vector<std::vector<std::string>> neighbor_tags;
    neighbor_tags.reserve(rows.size());
    for (const auto& row : rows) neighbor_tags.push_back(NeighborTags(row));
    const std::string prompt = BuildPrompt(settings_, vocabulary, full, rows, neighbor_tags);

    std::optional<nlohmann::json> answer;
    for (int attempt = 0; attempt < 2 && !answer; ++attempt) {
        std::vector<llm::Message> messages;
        messages.push_back(llm::Message{llm::Role::User, {llm::TextBlock{prompt}}});
        llm::ChatRequest request;
        request.model = llm->model;
        request.max_tokens = settings_.tagger_max_tokens;
        request.messages = messages;  // span; `messages` outlives the call
        request.stream = true;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(settings_.tagger_timeout_ms);
        try {
            // Streamed only so the sink can abort: on shutdown or past the
            // deadline, returning false cancels the transfer.
            const auto response = llm->provider->StreamComplete(request, [&](const llm::StreamEvent&) {
                std::lock_guard<std::mutex> lock(mu_);
                return !stop_ && std::chrono::steady_clock::now() < deadline;
            });
            if (response.stop_reason == llm::StopReason::Cancelled) {
                throw std::runtime_error("tagging call cancelled (shutdown or tagger_timeout_ms)");
            }
            if (const auto json = ExtractJsonObjectText(ResponseText(response))) {
                auto parsed = nlohmann::json::parse(*json);
                if (parsed.is_object() && parsed.contains("entries") && parsed["entries"].is_array()) {
                    answer = std::move(parsed);
                }
            }
        } catch (const std::exception& error) {
            // Transport/provider trouble: leave the rows pending, back off,
            // and re-resolve the model next time (a provider entry may have
            // changed).
            breaker_.RecordFailure(CircuitBreaker::Clock::now(), error.what());
            {
                std::lock_guard<std::mutex> lock(llm_mu_);
                llm_.reset();
            }
            logger_.warn("memory: tagging call failed, retrying later: {}", error.what());
            return false;
        }
    }
    breaker_.RecordSuccess();

    std::vector<std::string> ids;
    for (const auto& row : rows) ids.push_back(row.id);
    if (!answer) {
        logger_.warn("memory: tagger returned no usable JSON twice; {} entries left untagged",
                     rows.size());
        store_->SetTagStatus(ids, 2);
        return true;
    }

    std::map<std::size_t, nlohmann::json> by_index;
    for (const auto& element : (*answer)["entries"]) {
        if (!element.is_object()) continue;
        const auto n = element.value("n", 0);
        if (n >= 1 && static_cast<std::size_t>(n) <= rows.size()) {
            by_index[static_cast<std::size_t>(n - 1)] = element;
        }
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        std::vector<std::int64_t> tag_ids;
        auto add = [&](std::optional<std::int64_t> id) {
            if (id && std::find(tag_ids.begin(), tag_ids.end(), *id) == tag_ids.end() &&
                static_cast<int>(tag_ids.size()) < settings_.max_tags_per_entry) {
                tag_ids.push_back(*id);
            }
        };
        if (const auto it = by_index.find(i); it != by_index.end()) {
            const auto& element = it->second;
            if (const auto tags = element.find("tags"); tags != element.end() && tags->is_array()) {
                for (const auto& tag : *tags) {
                    if (!tag.is_string()) continue;
                    // An "existing" tag the model invented is a new-tag
                    // proposal without a description: same checks.
                    if (const auto found = store_->FindTag(scope, root, tag.get<std::string>())) {
                        add(found->id);
                    } else {
                        add(ResolveNewTag(scope, root, tag.get<std::string>(), ""));
                    }
                }
            }
            if (const auto fresh = element.find("new"); fresh != element.end() && fresh->is_array()) {
                for (const auto& proposal : *fresh) {
                    if (!proposal.is_object()) continue;
                    add(ResolveNewTag(scope, root, proposal.value("name", std::string()),
                                      proposal.value("description", std::string())));
                }
            }
        }
        // An entry the model skipped is filed with no tags rather than
        // retried forever.
        store_->SetEntryTags(rows[i].id, tag_ids);
    }
    return true;
}

std::vector<Tagger::MergeSuggestion> Tagger::SuggestMerges(int scope,
                                                           const std::string& project_root) {
    if (!enabled()) throw std::runtime_error("tagging is off (no tagger_model configured)");
    const auto vocabulary = store_->Tags(scope, project_root);
    if (vocabulary.size() < 2) return {};
    const auto llm = Llm();
    if (!llm || llm->provider == nullptr) {
        throw std::runtime_error("cannot resolve tagger_model '" + settings_.tagger_model + "'");
    }
    std::string prompt =
        "Below is a tag vocabulary for filing memory entries. Find tags that mean the same "
        "thing (synonyms, spelling or wording variants, one being a strict subset of another "
        "with too few entries to justify it). Merge the smaller into the better-named one. "
        "Do not merge tags that are merely related.\n\nTags (name: description [entries]):\n";
    for (const auto& tag : vocabulary) {
        prompt += "- " + tag.name + ": " + (tag.description.empty() ? "-" : tag.description) +
                  " [" + std::to_string(tag.count) + "]\n";
    }
    prompt += "\nReply with ONLY a JSON object: {\"merges\": [{\"from\": \"tag\", \"to\": "
              "\"tag\", \"why\": \"short reason\"}]} -- an empty list when nothing should merge.";
    std::vector<llm::Message> messages;
    messages.push_back(llm::Message{llm::Role::User, {llm::TextBlock{prompt}}});
    llm::ChatRequest request;
    request.model = llm->model;
    request.max_tokens = settings_.tagger_max_tokens;
    request.messages = messages;
    const auto response = llm->provider->Complete(request);
    const auto json = ExtractJsonObjectText(ResponseText(response));
    if (!json) throw std::runtime_error("the tagger model returned no usable JSON");
    const auto parsed = nlohmann::json::parse(*json);
    std::vector<MergeSuggestion> out;
    if (!parsed.contains("merges") || !parsed["merges"].is_array()) return out;
    for (const auto& merge : parsed["merges"]) {
        if (!merge.is_object()) continue;
        MergeSuggestion suggestion{merge.value("from", std::string()),
                                   merge.value("to", std::string()),
                                   merge.value("why", std::string())};
        // Only pairs that actually exist: the model may misspell or invent.
        const auto from = store_->FindTag(scope, project_root, suggestion.from);
        const auto to = store_->FindTag(scope, project_root, suggestion.to);
        if (!from || !to || from->id == to->id) continue;
        suggestion.from = from->name;
        suggestion.to = to->name;
        out.push_back(std::move(suggestion));
    }
    return out;
}

void Tagger::WorkerLoop() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            notified_ = false;
        }
        bool did_work = false;
        try {
            did_work = RunOnce();
        } catch (const std::exception& error) {
            logger_.warn("memory: tagging worker error: {}", error.what());
        }
        if (did_work) continue;
        auto wait = kIdlePoll;
        const auto state = breaker_.Snapshot(CircuitBreaker::Clock::now());
        if (state.open) {
            wait = std::min<std::chrono::milliseconds>(
                wait, std::chrono::duration_cast<std::chrono::milliseconds>(state.retry_in) +
                          std::chrono::milliseconds(50));
        }
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, wait, [this] { return stop_ || notified_; });
        if (stop_) return;
    }
}

std::string Tagger::Describe() const {
    if (!enabled()) return "off (entries stay untagged)";
    std::string out = "on (" + settings_.tagger_model + ")";
    const auto state = breaker_.Snapshot(CircuitBreaker::Clock::now());
    if (state.open) {
        out += " -- paused, retry in " + std::to_string(state.retry_in.count()) + "s: " +
               state.last_error;
    }
    return out;
}

}  // namespace minimal::memory
