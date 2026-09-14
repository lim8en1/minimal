// Session traversal (rewind) -- all four tiers in one file, because the
// feature's correctness is a CHAIN: the store's discarded-mark semantics, the
// loop's working-set surgery, the session's orchestration, and the wire. A
// break at any tier reads as a wrong conversation somewhere downstream.

#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fakes.hpp"
#include "session_events.hpp"
#include "minimal/agent/agent_loop.hpp"
#include "minimal/agent/session.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/rpc/session_wiring.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/session/session_reader.hpp"
#include "minimal/session/sqlite_history_store.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

// --- Store-tier scaffolding --------------------------------------------------

session::SessionSummary MakeSession(session::SessionDatabase& db) {
    return db.CreateSession("rewind-test", std::nullopt,
                            std::filesystem::temp_directory_path().string(), "test");
}

// One turn's worth: a real user turn and an assistant reply.
void AppendTurn(session::SessionDatabase& db, const std::string& id, const std::string& text) {
    db.AppendMessage(id, llm::Message{llm::Role::User, {llm::TextBlock{text}}});
    db.AppendMessage(id, llm::Message{llm::Role::Assistant, {llm::TextBlock{"re: " + text}}});
}

std::string TextAt(session::SessionDatabase& db, const std::string& id, std::int64_t seq) {
    const auto stored = db.LoadMessage(id, seq);
    REQUIRE(stored.has_value());
    const auto* text = std::get_if<llm::TextBlock>(&stored->message.content.front());
    REQUIRE(text != nullptr);
    return text->text;
}

// --- Loop-tier scaffolding (the agent_loop_test Harness, trimmed) ------------

struct LoopHarness {
    testing::FakeLlmProvider provider;
    tools::ToolRegistry registry;
    testing::AlwaysAllowPermissionChecker checker;
    testing::InMemoryHistoryStore store;
    std::unique_ptr<tools::ToolExecutor> executor;
    std::unique_ptr<agent::AgentLoop> loop;

    void Build(agent::LoopConfig config = {}, agent::Compactor compactor = {}) {
        agent::AgentPreset preset;
        preset.allowed_tools = {"*"};
        preset.provider.model_name = "test-model";
        preset.permission_mode = testing::AllowAllPolicy();

        executor = std::make_unique<tools::ToolExecutor>(registry, checker,
                                                         testing::AllowAllPolicy(),
                                                         tools::McpPermissionPolicy{},
                                                         tools::PermissionRunMode::Normal);
        agent::SessionContext session{common::DefaultLogger()};
        session.session_id = "s1";
        session.project_root = ".";

        loop = std::make_unique<agent::AgentLoop>(
            provider, *executor, registry, store, std::move(preset), config, std::move(session),
            [] { return false; }, [] { return false; }, [](const llm::StreamEvent&) { return true; },
            std::move(compactor));
    }
};

config::CompactionTriggerSettings FixedTriggerForTests(std::int64_t tokens) {
    config::CompactionTriggerSettings trigger;
    trigger.kind = config::CompactionTriggerSettings::Kind::FixedTokens;
    trigger.tokens = tokens;
    return trigger;
}

// Three turns with a compactor that folds eagerly (occupancy 15 per response,
// trigger 10): leaves the working set at [summary, b=2, two=3, c=4, three=5]
// with the checkpoint covering seq <= 1 -- the exact shape the boundary tests
// need.
void BuildCompactedLoop(LoopHarness& h) {
    h.provider.PushResponse(testing::TextResponse("one"));
    h.provider.PushResponse(testing::TextResponse("two"));
    h.provider.PushResponse(testing::TextResponse("three"));
    auto compactor = [](std::span<const llm::Message>, llm::LlmProvider&,
                        const agent::CompactionRequest&) {
        return agent::CompactionResult{
            llm::Message{llm::Role::User, {llm::TextBlock{"[summary]"}}}, 0};
    };
    h.Build(
        agent::LoopConfig{.auto_compact = true,
                          .compaction_trigger = FixedTriggerForTests(10)},
        compactor);
    h.loop->RunTurn(testing::UserText("a"));
    h.loop->RunTurn(testing::UserText("b"));
    h.loop->RunTurn(testing::UserText("c"));
}

// --- Session-tier scaffolding: a REAL SQLite store behind a live Session -----

struct SqliteSessionFixture {
    testing::TempDir arena;
    session::SessionDatabase db{arena.path() / "sessions.db"};
    session::SessionSummary row = MakeSession(db);

    tools::ToolRegistry registry;
    agent::SessionObserverRegistry observers{common::DefaultLogger()};
    agent::LoopConfig loop_config;
    testing::FakeLlmProvider* provider = nullptr;
    std::unique_ptr<agent::Session> session;
    std::unique_ptr<testing::SessionStream> stream;

    // The counting observer, registered before Start() so it sees everything.
    int rewind_notifications = 0;
    std::int64_t last_rewind_seq = -100;

    SqliteSessionFixture() {
        observers.Register(std::make_shared<CountingObserver>(this));
    }

    struct CountingObserver : agent::SessionObserver {
        explicit CountingObserver(SqliteSessionFixture* owner) : owner_(owner) {}
        void OnSessionRewound(const agent::SessionInfo&, std::int64_t seq) override {
            ++owner_->rewind_notifications;
            owner_->last_rewind_seq = seq;
        }
        SqliteSessionFixture* owner_;
    };

    void Script(llm::ChatResponse response) { provider->PushResponse(std::move(response)); }

    void Start() {
        auto owned_provider = std::make_unique<testing::FakeLlmProvider>();
        provider = owned_provider.get();

        auto owned_store =
            std::make_unique<session::SqliteHistoryStore>(db, row.session_id);

        agent::SessionParams params;
        params.session_id = row.session_id;
        params.name = row.name;
        params.agent_preset = "test";
        params.project_root = std::filesystem::path(row.project_root);
        // THE REAL READER: rewind validation and reload read through it.
        params.history_reader = std::make_shared<session::SessionReader>(db);
        rpc::FinishSessionWiring(params);

        agent::AgentPreset preset;
        preset.name = "test";
        preset.allowed_tools = {"*"};
        preset.permission_mode = testing::AllowAllPolicy();
        preset.run_mode = tools::PermissionRunMode::Normal;

        session = std::make_unique<agent::Session>(
            std::move(params), std::move(preset), std::move(owned_provider),
            std::move(owned_store), registry, loop_config, observers, common::DefaultLogger());
        stream = std::make_unique<testing::SessionStream>(*session);
    }

    // Runs one user turn to completion, scripted with the given reply.
    void RunTurn(const std::string& text, const std::string& reply) {
        Script(testing::TextResponse(reply));
        REQUIRE(session->SubmitTurn(text));
        REQUIRE(session->WaitForIdleForTesting(5s));
    }

    std::vector<v1::ServerEvent> DrainQueue() { return stream->Drain(); }

    const v1::SessionRewoundEvent* FindRewound(const std::vector<v1::ServerEvent>& events) {
        for (const auto& event : events) {
            if (event.has_session_rewound()) return &event.session_rewound();
        }
        return nullptr;
    }

    const v1::ErrorEvent* FindError(const std::vector<v1::ServerEvent>& events) {
        for (const auto& event : events) {
            if (event.has_error()) return &event.error();
        }
        return nullptr;
    }

    ~SqliteSessionFixture() {
        if (session) session->Terminate();
        // The observer dispatcher runs OnSessionRewound on its own thread;
        // without this join the counting observer's fields can be written
        // after (or during) this fixture's destruction -- the same teardown
        // race the §12.39 note documents for the wedged-observer test.
        observers.WaitForIdleForTesting();
    }
};

}  // namespace

// --- Store tier ----------------------------------------------------------------

TEST_CASE("rewind marks discard without deleting, and redo restores",
          "[rewind][store]") {
    testing::TempDir arena;
    session::SessionDatabase db(arena.path() / "sessions.db");
    const auto row = MakeSession(db);
    AppendTurn(db, row.session_id, "first");
    AppendTurn(db, row.session_id, "second");
    AppendTurn(db, row.session_id, "third");
    // seqs: first=0/1, second=2/3, third=4/5.

    const auto marked = db.SetDiscardedAfter(row.session_id, 2);
    CHECK(marked == 4);  // seqs 2..5, none previously marked

    // The rows survive -- that is what makes redo possible...
    const auto restored = db.UnmarkDiscarded(row.session_id);
    CHECK(restored == 4);
    CHECK(TextAt(db, row.session_id, 4) == "third");
    CHECK(db.LastMessageSeq(row.session_id) == 5);
    CHECK(db.GetSession(row.session_id)->message_count == 6);
}

TEST_CASE("discarded rows vanish from every read path", "[rewind][store]") {
    testing::TempDir arena;
    session::SessionDatabase db(arena.path() / "sessions.db");
    const auto row = MakeSession(db);
    AppendTurn(db, row.session_id, "first");
    AppendTurn(db, row.session_id, "second");
    AppendTurn(db, row.session_id, "third");

    REQUIRE(db.SetDiscardedAfter(row.session_id, 2) == 4);

    // Scrollback pages, resume loads, presence checks and the picker's count
    // all report the ACTIVE transcript only.
    CHECK_FALSE(db.LoadMessage(row.session_id, 2).has_value());
    CHECK(db.LoadMessage(row.session_id, 1).has_value());
    CHECK(db.LastMessageSeq(row.session_id) == 1);
    CHECK(db.GetSession(row.session_id)->message_count == 2);

    const auto page = db.GetMessagesPage(row.session_id, std::nullopt, 100);
    REQUIRE(page.messages.size() == 2);
    CHECK(page.messages.front().seq == 1);

    const session::SessionReader reader(db);
    CHECK(reader.LoadAll(row.session_id).size() == 2);
    CHECK(reader.LoadAfter(row.session_id, 0).size() == 1);
    CHECK(reader.ActiveLastSeq(row.session_id) == 1);
}

TEST_CASE("the first append after a rewind reclaims the tail and continues its seq",
          "[rewind][store][fork]") {
    testing::TempDir arena;
    session::SessionDatabase db(arena.path() / "sessions.db");
    const auto row = MakeSession(db);
    AppendTurn(db, row.session_id, "first");
    AppendTurn(db, row.session_id, "second");
    AppendTurn(db, row.session_id, "third");

    REQUIRE(db.SetDiscardedAfter(row.session_id, 2) == 4);

    // THE FORK: the new turn's seed appends at seq 2 -- the active sequence it
    // replaces -- and the discarded rows are gone for good.
    const auto seq =
        db.AppendMessage(row.session_id, llm::Message{llm::Role::User,
                                                      {llm::TextBlock{"edited"}}});
    CHECK(seq == 2);
    CHECK(TextAt(db, row.session_id, 2) == "edited");
    CHECK(db.LastMessageSeq(row.session_id) == 2);
    CHECK(db.GetSession(row.session_id)->message_count == 3);

    // And once the fork has landed, redo has nothing to restore.
    CHECK(db.UnmarkDiscarded(row.session_id) == 0);
}

TEST_CASE("a store opened across a rewind resumes the active transcript",
          "[rewind][store][resume]") {
    testing::TempDir arena;
    session::SessionDatabase db(arena.path() / "sessions.db");
    const auto row = MakeSession(db);
    AppendTurn(db, row.session_id, "first");
    AppendTurn(db, row.session_id, "second");
    REQUIRE(db.SetDiscardedAfter(row.session_id, 2) == 2);

    // A fresh HistoryStore -- what a resumed session constructs -- seeds its
    // seq accounting from the ACTIVE transcript, so its first append lands at
    // seq 2 and not above rows the user rewound away.
    session::SqliteHistoryStore store(db, row.session_id);
    CHECK(store.last_appended_seq() == 1);
    store.Append(llm::Message{llm::Role::User, {llm::TextBlock{"after rewind"}}}, std::nullopt, {});
    CHECK(store.last_appended_seq() == 2);
}

TEST_CASE("a v4 database migrates to v5 with its transcript intact",
          "[rewind][store][migration]") {
    testing::TempDir arena;
    const auto path = arena.path() / "migrate.db";

    // Hand-craft the v4 shape: no discarded column, a couple of rows.
    {
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
        const char* statements[] = {
            "CREATE TABLE sessions ("
            "  id TEXT PRIMARY KEY, parent_session_id TEXT REFERENCES sessions(id) ON DELETE"
            " CASCADE, name TEXT NOT NULL DEFAULT '', project_root TEXT NOT NULL,"
            " agent_preset TEXT NOT NULL, created_at INTEGER NOT NULL,"
            " updated_at INTEGER NOT NULL);",
            "CREATE TABLE messages ("
            "  id INTEGER PRIMARY KEY, session_id TEXT NOT NULL REFERENCES sessions(id) ON"
            " DELETE CASCADE, seq INTEGER NOT NULL, role TEXT NOT NULL,"
            " content_json TEXT NOT NULL, token_count INTEGER NOT NULL DEFAULT 0,"
            " created_at INTEGER NOT NULL, UNIQUE (session_id, seq));",
            "ALTER TABLE sessions ADD COLUMN message_count INTEGER NOT NULL DEFAULT 0;",
            "CREATE TABLE compactions ("
            "  session_id TEXT PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,"
            "  boundary_seq INTEGER NOT NULL, summary_content_json TEXT NOT NULL,"
            "  created_at INTEGER NOT NULL);",
            "INSERT INTO sessions (id, name, project_root, agent_preset, created_at,"
            " updated_at, message_count) VALUES ('old', 'old', '/tmp', 'test', 1, 1, 2);",
            "INSERT INTO messages (session_id, seq, role, content_json, token_count,"
            " created_at) VALUES ('old', 0, 'user', '{\"role\":\"user\",\"content\":"
            "[{\"type\":\"text\",\"text\":\"pre-migration\"}]}', 0, 1);",
            "INSERT INTO messages (session_id, seq, role, content_json, token_count,"
            " created_at) VALUES ('old', 1, 'assistant', '{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"kept\"}]}', 0, 1);",
            "PRAGMA user_version = 4;",
        };
        for (const char* sql : statements) {
            char* err = nullptr;
            REQUIRE(sqlite3_exec(raw, sql, nullptr, nullptr, &err) == SQLITE_OK);
            if (err) sqlite3_free(err);
        }
        sqlite3_close(raw);
    }

    session::SessionDatabase db(path);
    CHECK(db.LastMessageSeq("old") == 1);
    const auto stored = db.LoadMessage("old", 0);
    REQUIRE(stored.has_value());
    const auto* text = std::get_if<llm::TextBlock>(&stored->message.content.front());
    REQUIRE(text != nullptr);
    CHECK(text->text == "pre-migration");

    // And the new machinery works on the migrated rows.
    REQUIRE(db.SetDiscardedAfter("old", 0) == 2);
    CHECK(db.LastMessageSeq("old") == -1);
    REQUIRE(db.UnmarkDiscarded("old") == 2);
    CHECK(db.LastMessageSeq("old") == 1);
}

// --- Loop tier -----------------------------------------------------------------

TEST_CASE("TruncateAbove keeps the summary when the cut lands above the boundary",
          "[rewind][agent_loop]") {
    LoopHarness h;
    BuildCompactedLoop(h);
    // Working set: [summary, b=2, two=3, c=4, three=5]; boundary 1.
    REQUIRE(h.loop->CheckpointBoundary() == std::optional<std::int64_t>(1));

    h.loop->TruncateAfter(3);  // keep rows < 3: b stays raw

    const auto& history = h.loop->history();
    REQUIRE(history.size() == 2);
    const auto* head = std::get_if<llm::TextBlock>(&history[0].content.front());
    REQUIRE(head != nullptr);
    CHECK(head->text == "[summary]");
    const auto* tail = std::get_if<llm::TextBlock>(&history[1].content.front());
    REQUIRE(tail != nullptr);
    CHECK(tail->text == "b");
}

TEST_CASE("a rewind below the checkpoint boundary reseeds from the active prefix",
          "[rewind][agent_loop]") {
    LoopHarness h;
    BuildCompactedLoop(h);

    // The store's active prefix after the Session's mark IS what the loop gets
    // handed; here it is built directly to isolate the loop half.
    std::vector<llm::Message> prefix = {testing::UserText("a")};

    h.loop->ReseedFromStore(std::move(prefix), 0);

    const auto& history = h.loop->history();
    REQUIRE(history.size() == 1);
    CHECK(h.loop->CheckpointBoundary() == std::nullopt);
    // The reseeded set is what the next provider call sees, with no summary
    // pretending turns happened that did not.
    h.provider.PushResponse(testing::TextResponse("after rewind"));
    const auto result = h.loop->RunTurn(testing::UserText("next"));
    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    REQUIRE(h.provider.requests().size() == 4);
    const auto& sent = h.provider.requests().back().messages;
    REQUIRE(sent.size() == 2);  // the reseeded prefix + the new seed
    CHECK(std::get<llm::TextBlock>(sent[0].content.front()).text == "a");
}

TEST_CASE("TruncateAfter to the transcript start empties the working set",
          "[rewind][agent_loop]") {
    LoopHarness h;
    BuildCompactedLoop(h);

    h.loop->TruncateAfter(0);

    CHECK(h.loop->history().empty());
    CHECK(h.loop->CheckpointBoundary() == std::nullopt);
}

TEST_CASE("a rewound loop drops pending subagent completions", "[rewind][agent_loop]") {
    LoopHarness h;
    h.provider.PushResponse(testing::TextResponse("one"));
    h.provider.PushResponse(testing::TextResponse("two"));
    h.Build();

    REQUIRE_NOTHROW(h.loop->RunTurn(testing::UserText("a")));
    // A background subagent finished while the user was deciding to rewind.
    h.loop->PushInjectedMessage(testing::UserText("late completion"));

    h.loop->TruncateAfter(0);
    h.provider.PushResponse(testing::TextResponse("after rewind"));
    REQUIRE_NOTHROW(h.loop->RunTurn(testing::UserText("next")));

    // The completion answered a turn that no longer happened: it must not
    // surface in the provider request or the transcript.
    for (const auto& message : h.provider.requests().back().messages) {
        for (const auto& block : message.content) {
            const auto* text = std::get_if<llm::TextBlock>(&block);
            if (text != nullptr) CHECK(text->text != "late completion");
        }
    }
    for (const auto& message : h.loop->history()) {
        for (const auto& block : message.content) {
            const auto* text = std::get_if<llm::TextBlock>(&block);
            if (text != nullptr) CHECK(text->text != "late completion");
        }
    }
}

TEST_CASE("RestorePersisted joins rows without re-appending them", "[rewind][agent_loop]") {
    LoopHarness h;
    h.provider.PushResponse(testing::TextResponse("one"));
    h.provider.PushResponse(testing::TextResponse("two"));
    h.Build();
    REQUIRE_NOTHROW(h.loop->RunTurn(testing::UserText("a")));
    REQUIRE_NOTHROW(h.loop->RunTurn(testing::UserText("b")));
    // transcript: a=0, one=1, b=2, two=3
    const std::size_t stored_before = h.store.messages.size();

    h.loop->TruncateAfter(2);
    REQUIRE(h.loop->history().size() == 2);

    // REDO's memory half: the store restored rows 2..3; they join as-is.
    h.loop->RestorePersisted({testing::UserText("b"),
                              llm::Message{llm::Role::Assistant, {llm::TextBlock{"re: b"}}}},
                             2);

    REQUIRE(h.loop->history().size() == 4);
    CHECK(h.store.messages.size() == stored_before);  // nothing re-appended
    const auto* tail = std::get_if<llm::TextBlock>(&h.loop->history().back().content.front());
    REQUIRE(tail != nullptr);
    CHECK(tail->text == "re: b");
}

// --- Session tier ----------------------------------------------------------

TEST_CASE("a rewind truncates the transcript, the working set, and answers the wire",
          "[rewind][session]") {
    SqliteSessionFixture fx;
    fx.Start();
    fx.RunTurn("first", "re: first");
    fx.RunTurn("second", "re: second");
    // seqs: first=0, re=1, second=2, re=3.

    REQUIRE(fx.session->SubmitRewind(agent::QueuedRewind{2, false}));
    REQUIRE(fx.session->WaitForIdleForTesting(5s));

    const auto events = fx.DrainQueue();
    const auto* rewound = fx.FindRewound(events);
    REQUIRE(rewound != nullptr);
    CHECK(rewound->truncated_to_seq() == 2);
    CHECK(rewound->active_last_seq() == 1);
    CHECK_FALSE(fx.FindError(events) != nullptr);

    // The durable transcript ends before the cut...
    CHECK_FALSE(fx.db.LoadMessage(fx.row.session_id, 2).has_value());
    CHECK(fx.db.LastMessageSeq(fx.row.session_id) == 1);
    // ...and the observer notification fired, as it must for shadow-git. The
    // counters are written on the observer dispatcher thread, so drain it
    // before reading them.
    fx.observers.WaitForIdleForTesting();
    CHECK(fx.rewind_notifications == 1);
    CHECK(fx.last_rewind_seq == 2);

    // The next turn runs on the truncated context: prefix (2) + seed (1).
    fx.RunTurn("after rewind", "done");
    REQUIRE(fx.provider->requests().size() == 3);
    CHECK(fx.provider->requests()[2].messages.size() == 3);
    const auto& seed = fx.provider->requests()[2].messages.front();
    const auto* text = std::get_if<llm::TextBlock>(&seed.content.front());
    REQUIRE(text != nullptr);
    CHECK(text->text == "first");
}

TEST_CASE("a redo restores the discarded tail into transcript and working set",
          "[rewind][session]") {
    SqliteSessionFixture fx;
    fx.Start();
    fx.RunTurn("first", "re: first");
    fx.RunTurn("second", "re: second");

    REQUIRE(fx.session->SubmitRewind(agent::QueuedRewind{2, false}));
    REQUIRE(fx.session->WaitForIdleForTesting(5s));
    fx.DrainQueue();

    REQUIRE(fx.session->SubmitRewind(agent::QueuedRewind{-1, true}));
    REQUIRE(fx.session->WaitForIdleForTesting(5s));

    const auto events = fx.DrainQueue();
    const auto* rewound = fx.FindRewound(events);
    REQUIRE(rewound != nullptr);
    CHECK(rewound->truncated_to_seq() == -1);
    CHECK(rewound->active_last_seq() == 3);

    // The transcript is whole again, and the next turn runs on the FULL
    // context: 4 restored/kept messages + the new seed.
    CHECK(fx.db.LastMessageSeq(fx.row.session_id) == 3);
    fx.RunTurn("after redo", "done");
    REQUIRE(fx.provider->requests().size() == 3);
    CHECK(fx.provider->requests()[2].messages.size() == 5);
}

TEST_CASE("a rewind followed by a turn forks the branch for good",
          "[rewind][session][fork]") {
    SqliteSessionFixture fx;
    fx.Start();
    fx.RunTurn("first", "re: first");
    fx.RunTurn("second", "re: second");

    REQUIRE(fx.session->SubmitRewind(agent::QueuedRewind{2, false}));
    REQUIRE(fx.session->WaitForIdleForTesting(5s));
    fx.DrainQueue();

    fx.RunTurn("edited", "done");

    // The fork landed: seq 2 is the new seed and seq 3 its reply -- the old
    // branch is reclaimed.
    CHECK(fx.db.LastMessageSeq(fx.row.session_id) == 3);
    const auto stored = fx.db.LoadMessage(fx.row.session_id, 2);
    REQUIRE(stored.has_value());
    const auto* text = std::get_if<llm::TextBlock>(&stored->message.content.front());
    REQUIRE(text != nullptr);
    CHECK(text->text == "edited");

    // And redo is now a polite refusal, not a resurrection.
    REQUIRE(fx.session->SubmitRewind(agent::QueuedRewind{-1, true}));
    REQUIRE(fx.session->WaitForIdleForTesting(5s));
    const auto events = fx.DrainQueue();
    const auto* error = fx.FindError(events);
    REQUIRE(error != nullptr);
    CHECK(error->message() == "nothing to redo");
    CHECK(fx.FindRewound(events) == nullptr);
}

TEST_CASE("rewind refuses invalid targets on the wire, never with silence",
          "[rewind][session]") {
    SqliteSessionFixture fx;
    fx.Start();
    fx.RunTurn("first", "re: first");
    fx.RunTurn("second", "re: second");
    fx.DrainQueue();

    // An assistant message is not a prompt to re-enter.
    REQUIRE(fx.session->SubmitRewind(agent::QueuedRewind{1, false}));
    REQUIRE(fx.session->WaitForIdleForTesting(5s));
    auto events = fx.DrainQueue();
    const auto* error = fx.FindError(events);
    REQUIRE(error != nullptr);
    CHECK(error->message().find("not an active user message") != std::string::npos);
    CHECK(fx.FindRewound(events) == nullptr);

    // Out of range is a worker-side refusal; malformed is refused on the
    // reader before it ever queues. Both answer on the wire, neither queues.
    REQUIRE(fx.session->SubmitRewind(agent::QueuedRewind{99, false}));
    REQUIRE(fx.session->WaitForIdleForTesting(5s));
    CHECK(fx.FindError(fx.DrainQueue()) != nullptr);

    CHECK_FALSE(fx.session->SubmitRewind(agent::QueuedRewind{-1, false}));
    CHECK(fx.FindError(fx.DrainQueue()) != nullptr);

    // Nothing above was accepted: the transcript is untouched.
    CHECK(fx.db.LastMessageSeq(fx.row.session_id) == 3);
    fx.observers.WaitForIdleForTesting();
    CHECK(fx.rewind_notifications == 0);
}

TEST_CASE("rewinding into a discarded branch is refused", "[rewind][session]") {
    SqliteSessionFixture fx;
    fx.Start();
    fx.RunTurn("first", "re: first");
    fx.RunTurn("second", "re: second");
    fx.DrainQueue();

    REQUIRE(fx.session->SubmitRewind(agent::QueuedRewind{2, false}));
    REQUIRE(fx.session->WaitForIdleForTesting(5s));
    fx.DrainQueue();

    // Seq 2 was just discarded; it is no longer a position to rewind to. The
    // reader filters it, so the target reads as absent.
    REQUIRE(fx.session->SubmitRewind(agent::QueuedRewind{2, false}));
    REQUIRE(fx.session->WaitForIdleForTesting(5s));
    CHECK(fx.FindError(fx.DrainQueue()) != nullptr);

    // A deeper rewind to a still-active position is fine.
    REQUIRE(fx.session->SubmitRewind(agent::QueuedRewind{0, false}));
    REQUIRE(fx.session->WaitForIdleForTesting(5s));
    const auto events = fx.DrainQueue();
    const auto* rewound = fx.FindRewound(events);
    REQUIRE(rewound != nullptr);
    CHECK(rewound->active_last_seq() == -1);
    CHECK(fx.db.LastMessageSeq(fx.row.session_id) == -1);
}
