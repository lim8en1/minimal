// typesafe-advisor: settings parsing, the census+majority decision rule, and
// the retry/budget HTTP loop against a real loopback server (the tools-web
// pattern -- exactly the layer a fake would paper over). The advisor's
// threading contract itself (budget, abandonment, cancel, throw-fall-through)
// is proven in permission_advisor_test.cpp and is not re-proven here; what IS
// re-proven once, through the real loader, is that this plugin's Advise
// resolves an Ask over a real socket (session_service_test.cpp).
//
// The response fixtures in fixtures/typesafe-advisor/ were captured live
// against OpenRouter /api/alpha/decisions (see that directory's README).

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "deadline.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/net/http_client.hpp"
#include "minimal/tools/permission_advisor.hpp"

#include "advisor.hpp"
#include "decision.hpp"
#include "settings.hpp"
#include "typesafe_client.hpp"

using namespace minimal;
using namespace std::chrono_literals;
using minimal::typesafe_advisor::Action;
using minimal::typesafe_advisor::AskJev;
using minimal::typesafe_advisor::ChoiceAnswer;
using minimal::typesafe_advisor::ExtractActionAnswer;
using minimal::typesafe_advisor::JevOutcome;
using minimal::typesafe_advisor::ParseSettings;
using minimal::typesafe_advisor::ResolveDecision;
using minimal::typesafe_advisor::Settings;

namespace {

// One-shot loopback HTTP server, one response per accepted connection --
// mirrors tools_web_test.cpp's LoopbackServer. Records each request's body so
// the backend-shape assertions have something to read.
class LoopbackServer {
public:
    explicit LoopbackServer(std::vector<std::string> responses)
        : responses_(std::move(responses)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listen_fd_, 1) == 0);
        socklen_t length = sizeof(address);
        REQUIRE(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { Serve(); });
    }

    ~LoopbackServer() {
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/"; }

    const std::vector<std::string>& requests() const { return requests_; }

private:
    static void SendAll(int fd, const std::string& bytes) {
#ifdef SO_NOSIGPIPE
        int one = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        std::size_t sent = 0;
        while (sent < bytes.size()) {
#ifdef MSG_NOSIGNAL
            ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#else
            ssize_t n = ::write(fd, bytes.data() + sent, bytes.size() - sent);
#endif
            if (n <= 0) return;
            sent += static_cast<std::size_t>(n);
        }
    }

    void Serve() {
        timeval tv{5, 0};
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        for (const auto& response : responses_) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) return;
            std::string request;
            char buffer[8192];
            while (request.find("\r\n\r\n") == std::string::npos) {
                ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
                if (n <= 0) break;
                request.append(buffer, static_cast<std::size_t>(n));
            }
            requests_.push_back(request);
            SendAll(fd, response);
            ::close(fd);
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::vector<std::string> responses_;
    std::vector<std::string> requests_;  // one per served connection
    std::thread thread_;
};

std::string HttpResponseOf(int status, std::string_view status_text, std::string_view body,
                           std::vector<std::string> extra_headers = {}) {
    std::string out =
        "HTTP/1.1 " + std::to_string(status) + " " + std::string(status_text) + "\r\n";
    out += "Content-Type: application/json\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    for (const auto& header : extra_headers) out += header + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += std::string(body);
    return out;
}

nlohmann::json LoadFixture(const std::string& name) {
    const auto path = std::filesystem::path(TYPESAFE_FIXTURE_DIR) / name;
    std::ifstream stream(path);
    REQUIRE(stream.good());
    return nlohmann::json::parse(stream);
}

// A confident allow from the LIVE fixture (0.91 / 0.09): both gates clear with
// the default thresholds.
Settings FastSettings() {
    Settings settings;
    settings.endpoint = "";  // the test fills the loopback URL per server
    settings.jitter = 0.0;   // deterministic delays
    settings.backoff_initial_ms = 20;
    settings.backoff_max_ms = 50;
    settings.retry_after_cap_ms = 30;
    settings.budget_ms = 2000;
    settings.request_timeout_ms = 2000;
    return settings;
}

tools::PermissionRequest MakeRequest(std::string tool = "bash") {
    tools::PermissionRequest request;
    request.session_id = "s1";
    request.tool_name = std::move(tool);
    request.summary = "run `make test`";
    request.category = tools::ToolCategory::Execute;
    request.tool_input = nlohmann::json{{"command", "make test"}};
    request.disclosure.summary = "run make";
    request.disclosure.grants.push_back({"bash:/usr/bin/make", "/usr/bin/make *"});
    return request;
}

// The systemone body the client builds, as the server saw it (after the
// headers, before the connection closed).
nlohmann::json BodyOf(const std::string& raw_request) {
    auto split = raw_request.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    return nlohmann::json::parse(raw_request.substr(split + 4));
}

}  // namespace

// --- Settings ----------------------------------------------------------------

TEST_CASE("typesafe-advisor settings default and degrade", "[typesafe_advisor][settings]") {
    const auto defaults = ParseSettings(nlohmann::json::object());
    CHECK(defaults.backend == minimal::typesafe_advisor::Backend::Typesafe);
    CHECK(defaults.api_key_env == "TYPESAFE_API_KEY");
    CHECK(defaults.EffectiveEndpoint() == "https://api.typesafe.ai/v1/systemone");
    CHECK(defaults.EffectiveModel() == "jev-latest");
    CHECK(defaults.budget_ms == 2000);
    CHECK(defaults.request_timeout_ms == 2000);
    CHECK(defaults.max_retries == 2);
    CHECK(defaults.jitter == 0.25);
    CHECK(defaults.min_probability_allow == 0.75);
    CHECK(defaults.min_probability_deny == 0.85);
    CHECK(defaults.min_margin == 0.25);
    CHECK(defaults.questions.empty());

    // A non-object config degrades to defaults, like every bundled plugin.
    const auto degraded = ParseSettings(nlohmann::json::array());
    CHECK(degraded.api_key_env == "TYPESAFE_API_KEY");

    // Wrong-typed fields keep the default; the struct's own defaults survive.
    const auto wrong_types = ParseSettings(nlohmann::json{
        {"api_key_env", 42},
        {"budget_ms", "soon"},
        {"min_margin", "wide"},
    });
    CHECK(wrong_types.api_key_env == "TYPESAFE_API_KEY");
    CHECK(wrong_types.budget_ms == 2000);
    CHECK(wrong_types.min_margin == 0.25);
}

TEST_CASE("typesafe-advisor openrouter backend swaps endpoint, key env and model id",
          "[typesafe_advisor][settings]") {
    const auto settings = ParseSettings(nlohmann::json{{"backend", "openrouter"}});
    CHECK(settings.backend == minimal::typesafe_advisor::Backend::OpenRouter);
    CHECK(settings.api_key_env == "OPENROUTER_API_KEY");
    CHECK(settings.EffectiveEndpoint() == "https://openrouter.ai/api/alpha/decisions");
    CHECK(settings.EffectiveModel() == "~typesafe/jev-latest");

    // An explicit api_key_env survives on either backend.
    const auto pinned = ParseSettings(
        nlohmann::json{{"backend", "openrouter"}, {"api_key_env", "MY_KEY"}});
    CHECK(pinned.api_key_env == "MY_KEY");

    // An unknown backend name degrades to the default, not to a broken URL.
    const auto unknown = ParseSettings(nlohmann::json{{"backend", "anthropic"}});
    CHECK(unknown.backend == minimal::typesafe_advisor::Backend::Typesafe);
}

TEST_CASE("typesafe-advisor request_timeout is clamped to the budget", "[typesafe_advisor]") {
    const auto settings =
        ParseSettings(nlohmann::json{{"budget_ms", 500}, {"request_timeout_ms", 90000}});
    CHECK(settings.request_timeout_ms == 500);
    // Non-positive values fall back to the default rather than disabling.
    const auto zero = ParseSettings(nlohmann::json{{"budget_ms", 0}});
    CHECK(zero.budget_ms == 2000);
}

// --- Decision rule -----------------------------------------------------------

TEST_CASE("typesafe-advisor extracts the action answer from the live envelope",
          "[typesafe_advisor][decision]") {
    // The captured live responses are the shape contract: one parser, both
    // backends.
    const auto allow = LoadFixture("allow-response.json");
    auto answer = ExtractActionAnswer(allow);
    REQUIRE(answer != std::nullopt);
    CHECK(answer->choice == "allow");
    CHECK(answer->p_allow > 0.90);  // the captured 0.91, asserted loosely:
    CHECK(answer->p_review < 0.10); // the fixture is data, not a float-literal oracle

    // Not an envelope / not a choice / option outside the fixed set: all
    // nullopt, all later "abstain + warn", never a guess.
    CHECK(ExtractActionAnswer(nlohmann::json::object()) == std::nullopt);
    CHECK(ExtractActionAnswer(nlohmann::json{{"answers", {}}}) == std::nullopt);
    CHECK(ExtractActionAnswer(
              nlohmann::json{{"answers", {{"action", {{"type", "noul"}, {"noul", 0.9}}}}}}) ==
          std::nullopt);
    CHECK(ExtractActionAnswer(nlohmann::json{
              {"answers", {{"action", {{"choice", "explode"}, {"probabilities", {}}}}}}}) ==
          std::nullopt);
}

TEST_CASE("typesafe-advisor decision rule: census floor, majority margin, review",
          "[typesafe_advisor][decision]") {
    const double allow_floor = 0.75;
    const double deny_floor = 0.85;
    const double margin = 0.25;

    // Live fixture: allow 0.91, runner-up 0.09 -> allow.
    CHECK(ResolveDecision(ChoiceAnswer{"allow", 0.91, 0.0, 0.09}, allow_floor, deny_floor, margin)
              .action == Action::kAllow);
    // Live fixture: deny 1.0 -> deny.
    CHECK(ResolveDecision(ChoiceAnswer{"deny", 0.0, 1.0, 0.0}, allow_floor, deny_floor, margin)
              .action == Action::kDeny);
    // Live fixture: review 0.86 -> abstain, whatever the numbers.
    CHECK(ResolveDecision(ChoiceAnswer{"review", 0.12, 0.02, 0.86}, allow_floor, deny_floor,
                          margin)
              .action == Action::kAbstain);

    // Census: exactly at the floor acts; just below abstains.
    CHECK(ResolveDecision(ChoiceAnswer{"allow", 0.75, 0.0, 0.25}, allow_floor, deny_floor, margin)
              .action == Action::kAllow);
    CHECK(ResolveDecision(ChoiceAnswer{"allow", 0.74, 0.0, 0.26}, allow_floor, deny_floor, margin)
              .action == Action::kAbstain);
    // Deny's floor is stricter than allow's: the same numbers that allow a
    // call do not deny one.
    CHECK(ResolveDecision(ChoiceAnswer{"deny", 0.0, 0.80, 0.20}, allow_floor, deny_floor, margin)
              .action == Action::kAbstain);
    CHECK(ResolveDecision(ChoiceAnswer{"deny", 0.0, 0.85, 0.15}, allow_floor, deny_floor, margin)
              .action == Action::kDeny);

    // Majority: a clear floor-clearing winner acts; a near-tie abstains even
    // above the floor (the flip case the cookbook measured).
    CHECK(ResolveDecision(ChoiceAnswer{"allow", 0.80, 0.40, 0.0}, allow_floor, deny_floor, margin)
              .action == Action::kAllow);
    CHECK(ResolveDecision(ChoiceAnswer{"allow", 0.80, 0.60, 0.0}, allow_floor, deny_floor, margin)
              .action == Action::kAbstain);
    // Exactly at the margin acts (>= semantics): 0.75 - 0.50 = 0.25.
    CHECK(ResolveDecision(ChoiceAnswer{"allow", 0.75, 0.50, 0.0}, allow_floor, deny_floor, margin)
              .action == Action::kAllow);

    // Abstains name the gate that fired -- that is the log line's content.
    CHECK(ResolveDecision(ChoiceAnswer{"allow", 0.60, 0.30, 0.10}, allow_floor, deny_floor, margin)
              .abstain_reason.find("floor") != std::string::npos);
    CHECK(ResolveDecision(ChoiceAnswer{"allow", 0.80, 0.60, 0.00}, allow_floor, deny_floor, margin)
              .abstain_reason.find("margin") != std::string::npos);
    CHECK(ResolveDecision(ChoiceAnswer{"review", 0.1, 0.0, 0.9}, allow_floor, deny_floor, margin)
              .abstain_reason == "review");
}

// --- Retry / budget loop ------------------------------------------------------

TEST_CASE("typesafe-advisor healthy path: one attempt, parsed answer, no retries",
          "[typesafe_advisor][retry]") {
    const auto fixture = LoadFixture("allow-response.json").dump();
    LoopbackServer server({HttpResponseOf(200, "OK", fixture)});

    auto settings = FastSettings();
    settings.endpoint = server.url();
    const auto outcome = AskJev(net::HttpClient{}, settings, "k", nlohmann::json{{"s", 1}},
                                [] { return false; });

    REQUIRE(outcome.ok);
    CHECK(outcome.answered_model == "typesafe/jev-1.13-20260917");
    CHECK(outcome.attempts == 1);
    CHECK(outcome.abstain_reason.empty());
    REQUIRE(server.requests().size() == 1);

    // The body is the systemone shape for BOTH backends -- the model id VALUE
    // is what carries the backend namespace (default typesafe backend:
    // "jev-latest").
    const auto body = BodyOf(server.requests().front());
    CHECK(body.at("model") == "jev-latest");
    REQUIRE(body.contains("state"));
    REQUIRE(body.contains("questions"));
    CHECK(body.at("questions").contains("action"));
}

TEST_CASE("typesafe-advisor openrouter: same body shape, model id carries the namespace",
          "[typesafe_advisor][retry]") {
    const auto fixture = LoadFixture("allow-response.json").dump();
    LoopbackServer server({HttpResponseOf(200, "OK", fixture)});

    auto settings = FastSettings();
    settings.endpoint = server.url();
    // The openrouter backend: same body, different model id VALUE.
    settings.backend = minimal::typesafe_advisor::Backend::OpenRouter;
    const auto outcome = AskJev(net::HttpClient{}, settings, "k", nlohmann::json{{"s", 1}},
                                [] { return false; });

    REQUIRE(outcome.ok);
    CHECK(outcome.attempts == 1);
    REQUIRE(server.requests().size() == 1);
    const auto body = BodyOf(server.requests().front());
    CHECK(body.at("model") == "~typesafe/jev-latest");
    // ...and byte-identical shape otherwise: state and questions top-level.
    REQUIRE(body.contains("state"));
    REQUIRE(body.contains("questions"));
    CHECK(body.at("questions").contains("action"));
}

TEST_CASE("typesafe-advisor retries a 429 then answers", "[typesafe_advisor][retry]") {
    const auto fixture = LoadFixture("deny-response.json").dump();
    LoopbackServer server({HttpResponseOf(429, "Too Many Requests", R"({"error":{}})",
                                          {"Retry-After: 1"}),
                           HttpResponseOf(200, "OK", fixture)});

    auto settings = FastSettings();
    settings.endpoint = server.url();
    const auto outcome = AskJev(net::HttpClient{}, settings, "k", nlohmann::json::object(),
                                [] { return false; });

    REQUIRE(outcome.ok);  // the "Retry-After: 1" was clamped to retry_after_cap_ms, not honored raw
    CHECK(outcome.attempts == 2);
    CHECK(server.requests().size() == 2);
    CHECK(outcome.total < std::chrono::milliseconds(settings.budget_ms));
}

TEST_CASE("typesafe-advisor a huge backoff_initial_ms still clamps to backoff_max_ms",
          "[typesafe_advisor][retry]") {
    // Regression: `backoff_initial_ms * (1 << attempt)` was computed in a
    // plain `int`, which overflows (UB) for a realistic large configured
    // value well before the backoff_max_ms clamp below it ever runs -- the
    // clamp then sees a wrapped/garbage value instead of the real one. With
    // backoff_initial_ms already far past backoff_max_ms at attempt 0, every
    // retry delay must clamp to exactly backoff_max_ms; three retries should
    // take close to 3*backoff_max_ms, not hang or race through instantly on
    // a wrapped negative delay.
    const auto too_many = HttpResponseOf(429, "Too Many Requests", R"({"error":{}})");
    LoopbackServer server({too_many, too_many, too_many, too_many});

    auto settings = FastSettings();
    settings.endpoint = server.url();
    settings.backoff_initial_ms = 2000000000;  // near INT_MAX
    settings.backoff_max_ms = 50;
    settings.retry_after_cap_ms = 50;
    settings.max_retries = 3;
    const auto started = std::chrono::steady_clock::now();
    const auto outcome = AskJev(net::HttpClient{}, settings, "k", nlohmann::json::object(),
                                [] { return false; });
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK_FALSE(outcome.ok);
    CHECK(outcome.attempts == 4);
    // 3 backoffs at ~50ms each: comfortably bounded, and long enough that a
    // wrapped-to-near-zero delay (the old bug's likely outcome) would fail
    // the lower bound.
    CHECK(elapsed >= std::chrono::milliseconds(120));
    CHECK(elapsed < std::chrono::seconds(5));
}

TEST_CASE("typesafe-advisor gives up after max_retries and abstains", "[typesafe_advisor][retry]") {
    const auto too_many = HttpResponseOf(429, "Too Many Requests", R"({"error":{}})");
    LoopbackServer server({too_many, too_many, too_many, too_many});

    auto settings = FastSettings();
    settings.endpoint = server.url();
    const auto outcome = AskJev(net::HttpClient{}, settings, "k", nlohmann::json::object(),
                                [] { return false; });

    CHECK_FALSE(outcome.ok);
    CHECK(outcome.attempts == 3);  // first attempt + max_retries(2)
    CHECK(server.requests().size() == 3);
}

TEST_CASE("typesafe-advisor budget bounds attempts: a slow server is abandoned, not waited out",
          "[typesafe_advisor][retry][budget]") {
    // The server ACCEPTS and then holds the connection ~200ms before
    // answering; the per-attempt cap (clamped to the 50ms budget) aborts the
    // first attempt and the budget is spent, so the loop returns before the
    // server ever answers -- and retries cannot help inside a spent budget.
    // A plain TCP holder: accepts one connection, sleeps past the budget, then
    // closes without answering. It isolates the budget property from the HTTP
    // path -- whatever the transport decides (timeout -> retryable, or EOF ->
    // error), the loop must terminate inside the budget.
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listen_fd, 1) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    const int port = ntohs(address.sin_port);
    std::thread acceptor([listen_fd] {
        int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) return;
        std::this_thread::sleep_for(200ms);
        ::close(fd);
        ::close(listen_fd);
    });

    auto settings = FastSettings();
    settings.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/";
    settings.budget_ms = 50;
    // Constructed directly (not through ParseSettings), so the parse-time
    // clamp does not apply: set the per-attempt cap explicitly, below the
    // budget -- its abort is what ends the call before the server answers.
    settings.request_timeout_ms = 40;
    settings.max_retries = 5;  // retries cannot resurrect a spent budget

    const auto before = std::chrono::steady_clock::now();
    const auto outcome = AskJev(net::HttpClient{}, settings, "k", nlohmann::json::object(),
                                [] { return false; });
    const auto elapsed = std::chrono::steady_clock::now() - before;
    acceptor.join();

    CHECK_FALSE(outcome.ok);
    // The call returned well before the server's 200ms sleep elapsed: the
    // budget, not the server, decided when the advisor gave up.
    CHECK(elapsed < 200ms);
}

TEST_CASE("typesafe-advisor honors a large Retry-After only up to the cap, then the budget",
          "[typesafe_advisor][retry]") {
    LoopbackServer server({HttpResponseOf(429, "Too Many Requests", R"({"error":{}})",
                                          {"Retry-After: 30"}),
                           HttpResponseOf(200, "OK", LoadFixture("allow-response.json").dump())});

    auto settings = FastSettings();
    settings.endpoint = server.url();
    settings.retry_after_cap_ms = 40;

    const auto before = std::chrono::steady_clock::now();
    const auto outcome = AskJev(net::HttpClient{}, settings, "k", nlohmann::json::object(),
                                [] { return false; });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - before);

    // The 30s header was clamped to 40ms, the retry succeeded.
    REQUIRE(outcome.ok);
    CHECK(outcome.attempts == 2);
    CHECK(elapsed < 1000ms);
}

TEST_CASE("typesafe-advisor a cancel during the backoff aborts the wait immediately",
          "[typesafe_advisor][retry][cancel]") {
    LoopbackServer server({HttpResponseOf(429, "Too Many Requests", R"({"error":{}})")});

    auto settings = FastSettings();
    settings.endpoint = server.url();
    settings.backoff_initial_ms = 2000;  // one full slice the cancel must cut short
    settings.backoff_max_ms = 2000;
    settings.budget_ms = 5000;

    std::atomic<bool> cancelled{false};
    minimal::testing::BackgroundResult<JevOutcome> call(
        [&] { return AskJev(net::HttpClient{}, settings, "k", nlohmann::json::object(),
                            [&] { return cancelled.load(); }); });

    // Blocked inside the first backoff sleep...
    CHECK_FALSE(call.WaitFor(100ms));
    // ...until the cancel lands, and then promptly.
    cancelled.store(true);
    CHECK(call.WaitFor(2000ms));
    const auto outcome = call.Get();
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.abstain_reason == "cancelled");
    CHECK(server.requests().size() == 1);  // no further attempt fired
}

TEST_CASE("typesafe-advisor non-retryable statuses never retry", "[typesafe_advisor][retry]") {
    for (const int status : {400, 401, 403, 422}) {
        LoopbackServer server({HttpResponseOf(status, "Err", R"({"error":{"code":1}})"),
                               HttpResponseOf(200, "OK", "{}")});
        auto settings = FastSettings();
        settings.endpoint = server.url();
        const auto outcome = AskJev(net::HttpClient{}, settings, "k", nlohmann::json::object(),
                                    [] { return false; });
        INFO("status " << status);
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.attempts == 1);
        CHECK(server.requests().size() == 1);  // the 200 was never sent a request
    }
}

TEST_CASE("typesafe-advisor a 2xx body that is not the envelope abstains without retrying",
          "[typesafe_advisor][retry]") {
    LoopbackServer server({HttpResponseOf(200, "OK", R"({"hello": "world"})"),
                           HttpResponseOf(200, "OK", LoadFixture("allow-response.json").dump())});
    auto settings = FastSettings();
    settings.endpoint = server.url();
    const auto outcome =
        AskJev(net::HttpClient{}, settings, "k", nlohmann::json::object(), [] { return false; });
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.abstain_reason.find("action") != std::string::npos);
    CHECK(server.requests().size() == 1);
}

TEST_CASE("typesafe-advisor a refused connection abstains after exhausting retries",
          "[typesafe_advisor][retry]") {
    // A port with nothing listening: ConnectFailed on every attempt.
    auto settings = FastSettings();
    settings.endpoint = "http://127.0.0.1:1/";
    settings.backoff_initial_ms = 1;
    const auto outcome =
        AskJev(net::HttpClient{}, settings, "k", nlohmann::json::object(), [] { return false; });
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.attempts == settings.max_retries + 1);
}

// --- The advisor over the seam ------------------------------------------------

TEST_CASE("typesafe-advisor Advise resolves allow/deny/abstain from the live-shaped answers",
          "[typesafe_advisor][advise]") {
    struct Case {
        std::string fixture;
        Action expected;
        std::string reason_contains;
    };
    const std::vector<Case> cases{
        {"allow-response.json", Action::kAllow, ""},
        {"deny-response.json", Action::kDeny, ""},
        // review 0.86: a confident REVIEW is still a human decision.
        {"review-response.json", Action::kAbstain, "review"},
    };

    for (const auto& use : cases) {
        LoopbackServer server(
            {HttpResponseOf(200, "OK", LoadFixture(use.fixture).dump())});
        auto settings = FastSettings();
        settings.endpoint = server.url();
        minimal::typesafe_advisor::TypesafeAdvisor advisor(settings, "k", net::HttpClient{});

        const auto response = advisor.Advise(MakeRequest(), tools::AdvisorContext{});

        INFO(use.fixture);
        if (use.expected == Action::kAbstain) {
            CHECK(response == std::nullopt);
            continue;
        }
        REQUIRE(response != std::nullopt);
        if (use.expected == Action::kAllow) {
            CHECK(response->decision == tools::PermissionDecision::Allow);
            // Every disclosed key granted: auto mode's point, bounded by the
            // downstream re-filter.
            REQUIRE(response->granted_keys.size() == 1);
            CHECK(response->granted_keys[0] == "bash:/usr/bin/make");
            CHECK(response->deny_message.empty());
        } else {
            CHECK(response->decision == tools::PermissionDecision::Deny);
            CHECK(response->granted_keys.empty());
            // The model reads WHY: the message names the advisor, the model
            // that answered, and the confidence it acted on.
            CHECK(response->deny_message.find("auto-denied by typesafe-advisor") !=
                  std::string::npos);
            CHECK(response->deny_message.find("p(deny)=") != std::string::npos);
        }
    }
}

TEST_CASE("typesafe-advisor Advise abstains on transport failure without answering",
          "[typesafe_advisor][advise]") {
    auto settings = FastSettings();
    settings.endpoint = "http://127.0.0.1:1/";  // nothing listening
    settings.backoff_initial_ms = 1;
    minimal::typesafe_advisor::TypesafeAdvisor advisor(settings, "k", net::HttpClient{});

    const auto response = advisor.Advise(MakeRequest(), tools::AdvisorContext{});
    CHECK(response == std::nullopt);  // the human decides; the turn is never lost
}

TEST_CASE("typesafe-advisor Advise honors cancellation before any network call",
          "[typesafe_advisor][advise][cancel]") {
    LoopbackServer server({HttpResponseOf(200, "OK", LoadFixture("allow-response.json").dump())});
    auto settings = FastSettings();
    settings.endpoint = server.url();
    minimal::typesafe_advisor::TypesafeAdvisor advisor(settings, "k", net::HttpClient{});

    const auto response = advisor.Advise(
        MakeRequest(), tools::AdvisorContext{[] { return true; }});
    CHECK(response == std::nullopt);
    CHECK(server.requests().size() == 0);  // cancelled: the endpoint is never touched
}

TEST_CASE("typesafe-advisor operator-overridden questions reach the wire verbatim",
          "[typesafe_advisor][advise]") {
    LoopbackServer server({HttpResponseOf(200, "OK", LoadFixture("allow-response.json").dump())});
    auto settings = FastSettings();
    settings.endpoint = server.url();
    settings.questions = nlohmann::json{
        {"action",
         {{"type", "choice"},
          {"instructions", "House rules: approve this?"},
          {"criteria", {{"allow", nullptr}, {"deny", nullptr}, {"review", nullptr}}}}}};
    settings.extra_questions = nlohmann::json{
        {"audit", {{"type", "noul"}, {"instructions", "Does this touch secrets?"}}}};

    minimal::typesafe_advisor::TypesafeAdvisor advisor(settings, "k", net::HttpClient{});
    const auto response = advisor.Advise(MakeRequest(), tools::AdvisorContext{});
    REQUIRE(response != std::nullopt);  // the fixture's allow answer still resolves

    const auto body = BodyOf(server.requests().front());
    CHECK(body.at("questions").at("action").at("instructions") == "House rules: approve this?");
    CHECK(body.at("questions").contains("audit"));  // fan-out: same request, logged only
}
