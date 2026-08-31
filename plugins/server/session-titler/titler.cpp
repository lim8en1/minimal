#include "titler.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "minimal/agent/session_support_service.hpp"

namespace minimal::session_titler {
namespace {

// Mirrors the free default's cap (session_database.cpp kDefaultNameMaxChars):
// whatever names the session, it ends up the same shape.
constexpr int kTitleMaxCodePoints = 60;

const char* kDefaultPrompt =
    "Write a short title for a coding-assistant conversation that begins with the user "
    "message below. Reply with the title ONLY: 3-6 words, no quotes, no trailing period, "
    "no explanation.";

std::string FirstUserText(const std::string& session_id, const session::SessionReader& reader,
                          int char_budget) {
    // A fresh session's first turn has just produced its first rows; this load
    // is a handful of messages, not a transcript. -1 (before seq 0) means "the
    // whole thing, ascending".
    for (const auto& stored : reader.LoadAfter(session_id, -1)) {
        if (stored.message.role != llm::Role::User) continue;
        for (const auto& block : stored.message.content) {
            if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                if (text->text.empty()) continue;
                return text->text.substr(0, static_cast<std::size_t>(char_budget));
            }
        }
        // A tool-result-only user message is not a name; keep looking, exactly
        // like the free default rule does.
    }
    return {};
}

std::string ResponseText(const llm::ChatResponse& response) {
    for (const auto& block : response.content) {
        if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
            if (!text->text.empty()) return text->text;
        }
    }
    return {};
}

}  // namespace

std::string_view DefaultPrompt() { return kDefaultPrompt; }

TitlerConfig ParseTitlerConfig(const nlohmann::json& config) {
    TitlerConfig out;
    if (!config.is_object()) return out;
    if (auto it = config.find("model"); it != config.end() && it->is_string()) {
        out.model = it->get<std::string>();
    }
    if (auto it = config.find("max_tokens"); it != config.end() && it->is_number_integer()) {
        const int value = it->get<int>();
        if (value > 0) out.max_tokens = value;
    }
    if (auto it = config.find("input_char_budget"); it != config.end() && it->is_number_integer()) {
        const int value = it->get<int>();
        if (value > 0) out.input_char_budget = value;
    }
    if (auto it = config.find("prompt"); it != config.end() && it->is_string()) {
        out.prompt = it->get<std::string>();
    }
    return out;
}

std::string CleanTitle(std::string_view text) {
    // Trim.
    const auto is_space = [](unsigned char c) {
        return c < 0x80 && std::isspace(c) != 0;
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && is_space(static_cast<unsigned char>(text[end - 1]))) --end;
    std::string out(text.substr(begin, end - begin));

    // Unwrap one pair of matching quotes -- models wrap titles in quotes often
    // enough to make this worth four lines, and anything fancier is a rabbit hole.
    if (out.size() >= 2) {
        const char first = out.front();
        const char last = out.back();
        const bool wrapped = (first == '"' && last == '"') || (first == '\'' && last == '\'') ||
                             (first == '`' && last == '`');
        if (wrapped) out = out.substr(1, out.size() - 2);
    }

    // Collapse any internal newline run to a space: a multi-line reply is one
    // label, not several.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool pending_space = false;
    for (const unsigned char c : out) {
        if (c == '\n' || c == '\r' || c == '\t') {
            pending_space = !collapsed.empty();
        } else {
            if (pending_space) {
                collapsed += ' ';
                pending_space = false;
            }
            collapsed += static_cast<char>(c);
        }
    }

    // Cap at code-point granularity -- a byte cut would split a UTF-8 sequence
    // and render as U+FFFD in the tab bar.
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < collapsed.size() && chars < static_cast<std::size_t>(kTitleMaxCodePoints)) {
        const unsigned char c = static_cast<unsigned char>(collapsed[i]);
        std::size_t len;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else len = 1;
        if (i + len > collapsed.size()) len = collapsed.size() - i;
        i += len;
        ++chars;
    }
    return collapsed.substr(0, i);
}

TitlerObserver::TitlerObserver(TitlerConfig config, plugins::ServiceRegistry* services,
                               common::Logger& logger)
    : state_(std::make_shared<State>()) {
    // Field-by-field: State holds a mutex and a set, so it is neither copyable
    // nor movable, and an aggregate temporary would be a copy.
    state_->config = std::move(config);
    state_->services = services;
    state_->logger = &logger;
}

void TitlerObserver::OnTurnCompleted(const agent::SessionInfo& info,
                                     const agent::TurnSummary& summary) {
    // Subagents are named by their task; the parent's model already wrote that
    // text, and a second call to rephrase it buys nothing.
    if (!info.parent_session_id.empty()) return;
    // A fresh session's first turn starts at seq 0. A resumed session's first
    // new turn starts above the seeded history, and a background-wake turn
    // never seeds -- either way, not a session this process created, and
    // retitling it would spend tokens to overwrite a title that already exists.
    if (summary.first_seq != 0) return;

    if (!state_->titled.insert(info.session_id).second) return;

    // EVERYTHING below runs on the observer thread on purpose (see the class
    // comment): the turn just completed means the client may close its stream
    // at any instant, which terminates the session and erases it from the
    // registry -- so the transcript read and the provider/model resolution
    // happen HERE, while the session is still findable. Only the provider
    // call itself, which nothing here can bound, goes to the detached thread.
    std::string first_text;
    std::optional<agent::SessionLlm> llm;
    if (state_->services != nullptr) {
        void* raw = state_->services->Find(agent::kSessionSupportServiceName);
        if (raw != nullptr) {
            auto* support = static_cast<agent::SessionSupportService*>(raw);
            if (auto reader = support->HistoryFor(info.session_id); reader != nullptr) {
                first_text =
                    FirstUserText(info.session_id, *reader, state_->config.input_char_budget);
            }
            if (!first_text.empty()) {
                llm = state_->config.model.empty()
                          ? support->LlmForSession(info.session_id)
                          : support->ResolveModelRef(state_->config.model);
            }
        }
    }
    if (first_text.empty()) {
        // Nothing to title from (the default rule found none either, so the
        // session keeps its empty name). The one-shot entry stays: the first
        // turn is the only chance.
        state_->logger->debug("session {}: no first-turn text to title from",
                              info.session_id);
        return;
    }
    if (!llm) {
        state_->logger->debug("session {}: no provider available for titling",
                              info.session_id);
        return;
    }

    // The detached worker holds the State shared_ptr and the RESOLVED provider
    // -- never `this`, and never a session lookup: the observer may be
    // destroyed with the registry while a straggler call is still in flight,
    // and the session may be long gone from the registry by the time the
    // response lands. The rename degrades correctly either way.
    auto state = state_;
    auto provider = llm->provider;
    std::string model = llm->model;
    std::string session_id = info.session_id;
    std::thread([state, provider = std::move(provider), model = std::move(model),
                 session_id = std::move(session_id), text = std::move(first_text)]() mutable {
        void* raw = state->services->Find(agent::kSessionSupportServiceName);
        if (raw == nullptr) return;
        auto* support = static_cast<agent::SessionSupportService*>(raw);
        TitlerObserver::GenerateTitle(state->config, *state->logger, *provider, model,
                                      session_id, text, *support);
    }).detach();
}

void TitlerObserver::OnSessionEnded(const agent::SessionInfo& info) {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->titled.erase(info.session_id);
}

void TitlerObserver::RunJobNow(const std::string& session_id, std::string first_text) const {
    // Test seam: the whole pipeline synchronously, including the service
    // resolution the production path does on the observer thread.
    if (state_->services == nullptr) return;
    void* raw = state_->services->Find(agent::kSessionSupportServiceName);
    if (raw == nullptr) return;
    auto* support = static_cast<agent::SessionSupportService*>(raw);

    // The session's own model unless the config overrode it. nullopt on an
    // unknown session or unresolvable override: no title, log at debug, done.
    std::optional<agent::SessionLlm> llm;
    if (state_->config.model.empty()) {
        llm = support->LlmForSession(session_id);
    } else {
        llm = support->ResolveModelRef(state_->config.model);
    }
    if (!llm) {
        state_->logger->debug("session {}: no provider available for titling", session_id);
        return;
    }
    GenerateTitle(state_->config, *state_->logger, *llm->provider, llm->model, session_id,
                  std::move(first_text), *support);
}

void TitlerObserver::GenerateTitle(const TitlerConfig& config, common::Logger& logger,
                                   llm::LlmProvider& provider, const std::string& model,
                                   const std::string& session_id, const std::string& first_text,
                                   agent::SessionSupportService& support) {
    llm::ChatRequest request;
    request.model = model;
    request.system = config.prompt.empty() ? std::string{kDefaultPrompt} : config.prompt;
    request.max_tokens = config.max_tokens;
    std::vector<llm::Message> messages;
    messages.push_back(llm::Message{llm::Role::User, {llm::TextBlock{first_text}}});
    request.messages = messages;  // span; `messages` outlives the Complete call

    try {
        const llm::ChatResponse response = provider.Complete(request);
        const std::string title = CleanTitle(ResponseText(response));
        if (title.empty()) {
            logger.debug("session {}: titler produced no usable text", session_id);
            return;
        }
        support.RenameSession(session_id, title);
    } catch (const std::exception& error) {
        // A failed title degrades to the free default name -- never an error
        // event, never a surfaced failure. This whole plugin is cosmetic.
        logger.warn("session {}: titling failed: {}", session_id, error.what());
    } catch (...) {
        logger.warn("session {}: titling failed with an unrecognized exception", session_id);
    }
}

}  // namespace minimal::session_titler
