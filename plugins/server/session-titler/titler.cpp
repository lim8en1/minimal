#include "titler.hpp"

#include <cctype>
#include <exception>
#include <optional>
#include <utility>
#include <vector>

#include "minimal/common/utf8.hpp"

namespace minimal::session_titler {
namespace {

// Mirrors the free default's cap (session_database.cpp kDefaultNameMaxChars):
// whatever names the session, it ends up the same shape.
constexpr std::size_t kTitleMaxCodePoints = 60;

// Pending titles, not sessions: one entry per NEW session awaiting its title.
// A backlog this deep means the provider is wedged; dropping beyond it keeps
// the default name, which is the plugin's whole failure mode anyway.
constexpr std::size_t kJobQueueSize = 64;

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
                // Code points, not bytes: a byte cut could split a UTF-8
                // sequence inside the titling REQUEST.
                return common::TruncateCodePoints(text->text,
                                                  static_cast<std::size_t>(char_budget));
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

bool IsAsciiSpace(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x80 && std::isspace(u) != 0;
}

void Trim(std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && IsAsciiSpace(s[begin])) ++begin;
    while (end > begin && IsAsciiSpace(s[end - 1])) --end;
    s = s.substr(begin, end - begin);
}

// Removes one layer of the decoration models add despite being told not to.
// Returns whether anything came off, so the caller can peel nested layers
// ("**\"Title\"**").
bool StripDecoration(std::string& s) {
    // Markdown heading.
    if (!s.empty() && s.front() == '#') {
        s.erase(0, s.find_first_not_of('#'));
        return true;
    }
    // A "Title:" label.
    constexpr std::string_view kLabel = "title:";
    if (s.size() >= kLabel.size()) {
        bool match = true;
        for (std::size_t i = 0; i < kLabel.size() && match; ++i) {
            match = std::tolower(static_cast<unsigned char>(s[i])) == kLabel[i];
        }
        if (match) {
            s.erase(0, kLabel.size());
            return true;
        }
    }
    // One wrapping pair: bold, emphasis, or quotes. Inner ones stay.
    for (const std::string_view wrap : {"**", "*", "\"", "'", "`"}) {
        if (s.size() >= 2 * wrap.size() && s.starts_with(wrap) && s.ends_with(wrap)) {
            s = s.substr(wrap.size(), s.size() - 2 * wrap.size());
            return true;
        }
    }
    return false;
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
    // Collapse every whitespace run to one space and DROP other C0 control
    // bytes: a model reply carrying an ESC sequence would otherwise reach the
    // session name and from there the tab bar -- ANSI escapes in the UI,
    // courtesy of the provider.
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (IsAsciiSpace(ch)) {
            pending_space = !out.empty();
        } else if (c < 0x20 || c == 0x7F) {
            continue;  // control byte: dropped, not rendered
        } else {
            if (pending_space) {
                out += ' ';
                pending_space = false;
            }
            out += ch;
        }
    }

    // Bounded: each pass removes at least one byte, and real replies carry
    // two or three layers at most.
    for (int pass = 0; pass < 4; ++pass) {
        Trim(out);
        if (!StripDecoration(out)) break;
    }
    Trim(out);
    // One trailing period, not an ellipsis.
    if (out.size() >= 2 && out.back() == '.' && out[out.size() - 2] != '.') {
        out.pop_back();
        Trim(out);
    }

    // Code-point cap: a byte cut would render as U+FFFD in the tab bar.
    return common::TruncateCodePoints(out, kTitleMaxCodePoints);
}

TitlerObserver::TitlerObserver(TitlerConfig config, plugins::ServiceRegistry* services,
                               common::Logger& logger)
    : config_(std::move(config)),
      services_(services),
      logger_(logger),
      jobs_(kJobQueueSize),
      worker_([this] { RunWorker(); }) {}

TitlerObserver::~TitlerObserver() {
    // Queued titles are dropped, not drained: N provider calls must not stand
    // between the host and exit. Only the in-flight call, if any, is waited on.
    stopping_.store(true, std::memory_order_release);
    jobs_.DiscardPending();
    jobs_.Close();
    if (worker_.joinable()) worker_.join();
}

void TitlerObserver::RunWorker() {
    while (auto session_id = jobs_.Pop()) {
        if (stopping_.load(std::memory_order_acquire)) return;
        ProcessJob(*session_id);
    }
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
    if (!titled_.insert(info.session_id).second) return;

    if (!jobs_.TryPush(info.session_id)) {
        logger_.debug("session {}: titling queue full or closed; keeping the default name",
                      info.session_id);
    }
}

void TitlerObserver::OnSessionEnded(const agent::SessionInfo& info) {
    titled_.erase(info.session_id);
}

void TitlerObserver::ProcessJob(const std::string& session_id) const {
    try {
        if (services_ == nullptr) return;
        auto* support = static_cast<agent::SessionSupportService*>(
            services_->Find(agent::kSessionSupportServiceName));
        if (support == nullptr) return;

        std::string first_text;
        if (const auto reader = support->HistoryFor(session_id); reader != nullptr) {
            first_text = FirstUserText(session_id, *reader, config_.input_char_budget);
        }
        if (first_text.empty()) {
            // Nothing to title from (the default rule found none either, so the
            // session keeps its empty name).
            logger_.debug("session {}: no first-turn text to title from", session_id);
            return;
        }

        // The session's own model unless the config overrode it. nullopt on an
        // unknown session or an unresolvable override: no title.
        const std::optional<agent::SessionLlm> llm =
            config_.model.empty() ? support->LlmForSession(session_id)
                                  : support->ResolveModelRef(config_.model);
        if (!llm || llm->provider == nullptr) {
            logger_.debug("session {}: no provider available for titling", session_id);
            return;
        }
        GenerateTitle(config_, logger_, *llm->provider, llm->model, session_id, first_text,
                      *support);
    } catch (const std::exception& error) {
        // A failed title degrades to the free default name -- never an error
        // event, never a surfaced failure. This whole plugin is cosmetic.
        logger_.warn("session {}: titling failed: {}", session_id, error.what());
    } catch (...) {
        logger_.warn("session {}: titling failed with an unrecognized exception", session_id);
    }
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

    const llm::ChatResponse response = provider.Complete(request);
    const std::string title = CleanTitle(ResponseText(response));
    if (title.empty()) {
        if (response.stop_reason == llm::StopReason::MaxTokens) {
            // The reasoning-model case: hidden reasoning spent the whole cap
            // before any visible text. Silent at debug, this looked like the
            // plugin doing nothing.
            logger.warn("session {}: titling hit max_tokens ({}) with no visible text; "
                        "raise session-titler's max_tokens or configure a non-reasoning model",
                        session_id, config.max_tokens);
        } else {
            logger.debug("session {}: titler produced no usable text", session_id);
        }
        return;
    }
    support.RenameSession(session_id, title);
}

}  // namespace minimal::session_titler
