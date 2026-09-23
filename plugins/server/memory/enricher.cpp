#include "enricher.hpp"

#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "memory_store.hpp"

#include "minimal/agent/session_support_service.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/llm/provider.hpp"

namespace minimal::memory {
namespace {

constexpr const char* kDefaultPrompt =
    "You file one memory entry. Reply with ONLY a JSON object, no other text:\n"
    "{\"summary\": \"<= 1 sentence, the durable lesson\", "
    "\"topic\": \"<existing topic name>\" | {\"new\": \"<short topic name>\"} | null}\n"
    "Prefer an existing topic from the list when one fits. Use null only when nothing fits.";

// Code-point budget on the REQUEST text, titler's rule: a byte cut could
// split a UTF-8 sequence inside the summarization request.
std::string TruncateCodePoints(std::string_view text, std::size_t max_chars) {
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

std::string ResponseText(const llm::ChatResponse& response) {
    for (const auto& block : response.content) {
        if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
            if (!text->text.empty()) return text->text;
        }
    }
    return {};
}

// Pull the first parseable {...} object out of the response -- models add
// prose or code fences despite instructions; strictness is about the
// OBJECT's shape, not the envelope.
std::optional<nlohmann::json> ExtractJsonObject(std::string_view text) {
    const auto begin = text.find('{');
    if (begin == std::string_view::npos) return std::nullopt;
    for (std::size_t end = text.size() + 1; end > begin + 1; --end) {
        if (text[end - 1] != '}') continue;
        try {
            auto parsed = nlohmann::json::parse(text.substr(begin, end - begin));
            if (parsed.is_object()) return parsed;
        } catch (const std::exception&) {
            // keep shrinking
        }
    }
    return std::nullopt;
}

// The one provider call. Returns the parsed object or nullopt (malformed
// after one retry, provider error). provider may be null: silent degrade.
std::optional<nlohmann::json> AskSummarizer(const MemorySettings& settings,
                                            llm::LlmProvider* provider, const std::string& model,
                                            const std::string& text,
                                            const std::vector<std::string>& topics,
                                            common::Logger& logger) {
    if (provider == nullptr) return std::nullopt;

    std::string topic_list;
    for (const auto& topic : topics) {
        if (!topic_list.empty()) topic_list += ", ";
        topic_list += topic;
    }
    if (topic_list.empty()) topic_list = "(none yet)";

    std::string prompt = kDefaultPrompt;
    prompt += "\n\nExisting topics: ";
    prompt += topic_list;
    prompt += "\n\nMemory entry:\n";
    prompt += TruncateCodePoints(text, static_cast<std::size_t>(settings.summarize_input_char_budget));

    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            llm::ChatRequest request;
            request.model = model;
            request.max_tokens = settings.summarize_max_tokens;
            std::vector<llm::Message> messages;
            messages.push_back(llm::Message{llm::Role::User, {llm::TextBlock{prompt}}});
            request.messages = messages;  // span; `messages` outlives Complete

            const llm::ChatResponse response = provider->Complete(request);
            if (auto parsed = ExtractJsonObject(ResponseText(response))) {
                return parsed;
            }
        } catch (const std::exception& error) {
            logger.warn("memory: enrichment call failed: {}", error.what());
            return std::nullopt;  // transport/provider errors do not retry
        }
    }
    return std::nullopt;  // malformed twice: unfiled, no summary
}

}  // namespace

struct Enricher::State {
    MemorySettings config;
    common::Logger logger;
    // The provider/model resolved for the CURRENT session by the tool layer
    // before Enqueue (titler's rule: resolve while the session exists). The
    // shared_ptr keeps the provider alive for the detached workers, and
    // swapping it is how a session/model switch propagates.
    std::mutex llm_mu;
    std::shared_ptr<llm::LlmProvider> provider;
    std::string model;
    std::mutex mu;
    std::condition_variable cv;
    std::size_t workers = 0;
    bool draining = false;
    // The store outlives the enricher (both live in the plugin's static
    // holder, store first); raw pointer is safe for the drain contract.
    MemoryStore* store = nullptr;
};

Enricher::Enricher(MemorySettings settings, MemoryStore* store) {
    state_ = std::make_shared<State>();
    state_->config = std::move(settings);
    state_->store = store;
    state_->logger = common::DefaultLogger();
}

// The tool layer resolves the model ref through SessionSupportService on the
// tool thread (the session exists there) and hands it over here. nullopt-ref
// clears: enrichment silently degrades for that stretch.
void Enricher::SetLlm(std::shared_ptr<llm::LlmProvider> provider, std::string model) {
    std::lock_guard<std::mutex> lock(state_->llm_mu);
    state_->provider = std::move(provider);
    state_->model = std::move(model);
}

void Enricher::Enqueue(std::string entry_id, std::string text, int scope,
                       std::string project_root, std::vector<std::string> also_bind) {
    if (!enabled() || entry_id.empty() || text.empty()) return;
    auto state = state_;
    std::shared_ptr<llm::LlmProvider> provider;
    std::string model;
    {
        std::lock_guard<std::mutex> lock(state->llm_mu);
        provider = state->provider;
        model = state->model;
    }
    if (provider == nullptr) return;  // nothing resolved: silent degrade

    {
        std::lock_guard<std::mutex> lock(state->mu);
        if (state->draining) return;
        state->workers += 1;
    }
    std::thread([state, provider = std::move(provider), model = std::move(model),
                 id = std::move(entry_id), text = std::move(text), scope,
                 root = std::move(project_root), also_bind = std::move(also_bind)]() mutable {
        // Enrichment is garnish (enricher.hpp's "failure is always
        // silent-degrade" invariant): a SQLite write failure in
        // RunEnrichment (SetSummary/BindTopic) must not escape a detached
        // thread, where an uncaught exception would call std::terminate and
        // take the whole process down, nor skip the decrement below, which
        // would hang Drain() forever.
        try {
            RunEnrichment(state->config, *state->store, provider.get(), model, state->logger, id,
                          text, scope, root, also_bind);
        } catch (const std::exception& error) {
            state->logger.warn("memory: enrichment worker failed: {}", error.what());
        }
        {
            std::lock_guard<std::mutex> lock(state->mu);
            state->workers -= 1;
        }
        state->cv.notify_all();
    }).detach();
}

// The pipeline, synchronous -- the testing seam. Ask, parse, attribute,
// write. Every failure path ends in "entry keeps preview, stays unfiled".
void Enricher::RunEnrichment(const MemorySettings& settings, MemoryStore& store,
                             llm::LlmProvider* provider, const std::string& model,
                             common::Logger& logger, const std::string& entry_id,
                             const std::string& text, int scope,
                             const std::string& project_root,
                             const std::vector<std::string>& also_bind) {
    const auto topics = store.TopicNames(scope, project_root, /*cap=*/200);
    const auto answer = AskSummarizer(settings, provider, model, text, topics, logger);
    if (!answer) return;

    // Summary: one sentence, trimmed; empty stays absent (preview remains).
    std::string summary;
    if (auto it = answer->find("summary"); it != answer->end() && it->is_string()) {
        summary = it->get<std::string>();
        while (!summary.empty() && (summary.front() == ' ' || summary.front() == '\n')) {
            summary.erase(summary.begin());
        }
        while (!summary.empty() && (summary.back() == ' ' || summary.back() == '\n')) {
            summary.pop_back();
        }
    }
    if (!summary.empty()) {
        store.SetSummary(entry_id, summary);
        for (const auto& sibling_id : also_bind) store.SetSummary(sibling_id, summary);
    }

    // Topic: existing (case-insensitive), {"new": ...} under cap, else null ->
    // unfiled (topic_id stays NULL -- no sentinel row, by design). Every
    // sibling gets the SAME resolved name/create decision -- one provider
    // call stands in for the whole ingest group, so a second sibling must
    // not re-roll "create" and end up with its own topic row.
    if (auto it = answer->find("topic"); it != answer->end()) {
        std::string topic_name;
        bool create = false;
        if (it->is_string()) {
            topic_name = it->get<std::string>();
        } else if (it->is_object()) {
            if (auto nw = it->find("new"); nw != it->end() && nw->is_string()) {
                topic_name = nw->get<std::string>();
                create = true;
            }
        }
        if (!topic_name.empty() && topic_name.size() <= 64) {
            store.BindTopic(entry_id, scope, project_root, topic_name, create, settings.max_topics);
            for (const auto& sibling_id : also_bind) {
                // The topic now exists (or already did): never create twice.
                store.BindTopic(sibling_id, scope, project_root, topic_name, /*create_if_missing=*/false,
                                settings.max_topics);
            }
        }
    }
}

void Enricher::Drain() {
    auto state = state_;
    std::unique_lock<std::mutex> lock(state->mu);
    state->draining = true;
    state->cv.wait(lock, [state] { return state->workers == 0; });
}

Enricher::~Enricher() { Drain(); }

}  // namespace minimal::memory
