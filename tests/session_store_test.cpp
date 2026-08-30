#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <variant>
#include <vector>

#include "minimal/llm/message.hpp"
#include "minimal/session/message_codec.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/session/session_reader.hpp"
#include "minimal/session/sqlite_history_store.hpp"
#include "minimal/session/ulid.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace minimal::session;
using minimal::testing::TempDir;

namespace {

// --- Structural equality (test-local; the core types deliberately carry none) ---

bool BlocksEqual(const llm::ContentBlock& a, const llm::ContentBlock& b) {
    // std::visit instantiates the lambda for every alternative pair, so the
    // mixed-type case must be a discarded branch rather than a runtime check.
    return std::visit(
        [](const auto& x, const auto& y) -> bool {
            using TX = std::decay_t<decltype(x)>;
            using TY = std::decay_t<decltype(y)>;
            if constexpr (!std::is_same_v<TX, TY>) {
                return false;
            } else if constexpr (std::is_same_v<TX, llm::TextBlock>) {
                return x.text == y.text;
            } else if constexpr (std::is_same_v<TX, llm::ThinkingBlock>) {
                return x.text == y.text && x.signature == y.signature;
            } else if constexpr (std::is_same_v<TX, llm::ToolUseBlock>) {
                return x.id == y.id && x.name == y.name && x.input == y.input;
            } else {  // ToolResultBlock
                if (x.tool_use_id != y.tool_use_id || x.is_error != y.is_error ||
                    x.content.size() != y.content.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < x.content.size(); ++i) {
                    const auto* xa = std::get_if<llm::TextBlock>(&x.content[i]);
                    const auto* xb = std::get_if<llm::TextBlock>(&y.content[i]);
                    if (!xa || !xb || xa->text != xb->text) return false;
                }
                return true;
            }
        },
        a, b);
}

bool MessagesEqual(const llm::Message& a, const llm::Message& b) {
    if (a.role != b.role || a.content.size() != b.content.size()) return false;
    for (std::size_t i = 0; i < a.content.size(); ++i) {
        if (!BlocksEqual(a.content[i], b.content[i])) return false;
    }
    return true;
}

llm::Message RoundTrip(const llm::Message& message) {
    return JsonToMessage(MessageToJson(message));
}

// A user message with one text block -- the workhorse of these tests.
llm::Message UserMsg(std::string text) {
    return llm::Message{llm::Role::User, {llm::TextBlock{std::move(text)}}};
}

// --- Raw SQLite access for fixtures and internal assertions -----------------
// SessionDatabase deliberately exposes none of this; the tests need it to build
// a hand-crafted newer-version database and to check integrity/journal mode.

sqlite3* RawOpen(const std::filesystem::path& path) {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    return db;
}

std::string RawScalar(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* db = RawOpen(path);
    std::string out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            if (const unsigned char* text = sqlite3_column_text(stmt, 0)) {
                out = reinterpret_cast<const char*>(text);
            }
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return out;
}

std::set<std::string> RawTableNames(const std::filesystem::path& path) {
    sqlite3* db = RawOpen(path);
    std::set<std::string> names;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table';", -1, &stmt,
                           nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            names.insert(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return names;
}

// Save/restore one environment variable around a test.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name_)) saved_ = old;
        setenv(name_, value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (!saved_.empty()) setenv(name_, saved_.c_str(), 1);
        else unsetenv(name_);
    }

private:
    const char* name_;
    std::string saved_;
};

}  // namespace

// --- ULID --------------------------------------------------------------------

TEST_CASE("ulids are 26 Crockford base32 characters", "[session][ulid]") {
    const std::string alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    for (int i = 0; i < 1000; ++i) {
        const std::string ulid = GenerateUlid();
        REQUIRE(ulid.size() == 26);
        for (const char c : ulid) {
            CHECK(alphabet.find(c) != std::string::npos);
        }
    }
}

TEST_CASE("ulids are unique", "[session][ulid]") {
    std::unordered_set<std::string> seen;
    for (int i = 0; i < 10000; ++i) seen.insert(GenerateUlid());
    REQUIRE(seen.size() == 10000);
}

TEST_CASE("ulids sort by creation time", "[session][ulid]") {
    const std::string a = GenerateUlid();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const std::string b = GenerateUlid();
    // The Crockford alphabet's character order matches its value order, so a
    // later timestamp always sorts after an earlier one as a plain string.
    CHECK(a < b);
}

// --- Message codec ------------------------------------------------------------

TEST_CASE("codec round-trips every content block variant", "[session][codec]") {
    llm::Message message{llm::Role::Assistant, {}};
    message.content.push_back(llm::TextBlock{"plain text with \"quotes\" and \\ backslash"});
    message.content.push_back(
        llm::ThinkingBlock{"reasoning here", std::string("sig-abc")});
    message.content.push_back(
        llm::ToolUseBlock{"toolu_1", "bash",
                          {{"command", "make 2>&1 | tail -50"}, {"timeout_ms", 30000}}});
    llm::ToolResultBlock result;
    result.tool_use_id = "toolu_1";
    result.content.push_back(llm::TextBlock{"target.cpp"});
    result.is_error = false;
    message.content.push_back(result);

    CHECK(MessagesEqual(RoundTrip(message), message));
}

TEST_CASE("codec round-trips thinking without a signature", "[session][codec]") {
    const llm::Message message{llm::Role::Assistant, {llm::ThinkingBlock{"thought", std::nullopt}}};
    CHECK(MessagesEqual(RoundTrip(message), message));
}

TEST_CASE("codec preserves arbitrary tool input JSON verbatim", "[session][codec]") {
    const nlohmann::json input = {
        {"nested", {{"deeper", "unicode: 日本語 — emoji 🎉"}}},
        {"list", {1, -2, 3.5, true, nullptr}},
        {"empty_obj", nlohmann::json::object()},
        {"big", 9007199254740993LL},
    };
    const llm::Message message{llm::Role::Assistant,
                               {llm::ToolUseBlock{"id1", "tool", input}}};
    const llm::Message back = RoundTrip(message);
    CHECK(MessagesEqual(back, message));
    const auto* tool_use = std::get_if<llm::ToolUseBlock>(&back.content[0]);
    REQUIRE(tool_use != nullptr);
    CHECK(tool_use->input == input);
}

TEST_CASE("codec round-trips error tool results with multiple text blocks", "[session][codec]") {
    llm::ToolResultBlock result;
    result.tool_use_id = "toolu_2";
    result.content.push_back(llm::TextBlock{"line one"});
    result.content.push_back(llm::TextBlock{"line two"});
    result.is_error = true;
    const llm::Message message{llm::Role::User, {result}};
    CHECK(MessagesEqual(RoundTrip(message), message));
}

TEST_CASE("codec round-trips an empty-content message", "[session][codec]") {
    const llm::Message message{llm::Role::User, {}};
    CHECK(MessagesEqual(RoundTrip(message), message));
}

TEST_CASE("codec refuses unknown block types on decode", "[session][codec]") {
    nlohmann::json doc = {{"role", "user"},
                          {"content", {{{"type", "warp_drive"}, {"text", "hi"}}}}};
    CHECK_THROWS_AS(JsonToMessage(doc), std::runtime_error);
}

TEST_CASE("codec refuses malformed blocks on decode", "[session][codec]") {
    // Missing required field.
    nlohmann::json missing = {{"role", "user"}, {"content", {{{"type", "text"}}}}};
    CHECK_THROWS_AS(JsonToMessage(missing), std::runtime_error);
    // Wrong type for a field.
    nlohmann::json wrong_type = {{"role", "user"},
                                 {"content", {{{"type", "tool_use"}, {"id", 42},
                                               {"name", "x"}, {"input", nlohmann::json::object()}}}}};
    CHECK_THROWS_AS(JsonToMessage(wrong_type), std::runtime_error);
    // Unknown role.
    nlohmann::json bad_role = {{"role", "system"}, {"content", nlohmann::json::array()}};
    CHECK_THROWS_AS(JsonToMessage(bad_role), std::runtime_error);
    // Tool result content holding a non-text block.
    nlohmann::json nested = {{"role", "user"},
                             {"content",
                              {{{"type", "tool_result"},
                                {"tool_use_id", "t"},
                                {"is_error", false},
                                {"content", {{{"type", "tool_use"}, {"id", "x"},
                                              {"name", "y"}, {"input", nlohmann::json::object()}}}}}}}};
    CHECK_THROWS_AS(JsonToMessage(nested), std::runtime_error);
}

// --- Database path resolution --------------------------------------------------

TEST_CASE("empty session_db_path resolves to DataDir/sessions.db", "[session][path]") {
    TempDir dir;
    ScopedEnv env("XDG_DATA_HOME", dir.path().string());
    const auto resolved = ResolveSessionDbPath("");
    CHECK(resolved == dir.path() / "minimal" / "sessions.db");
}

TEST_CASE("relative session_db_path resolves against DataDir, not the cwd", "[session][path]") {
    TempDir dir;
    ScopedEnv env("XDG_DATA_HOME", dir.path().string());
    const auto resolved = ResolveSessionDbPath("store/sessions.db");
    CHECK(resolved == dir.path() / "minimal" / "store" / "sessions.db");
}

TEST_CASE("absolute session_db_path is used as-is", "[session][path]") {
    TempDir dir;
    const auto resolved = ResolveSessionDbPath((dir.path() / "elsewhere.db").string());
    CHECK(resolved == dir.path() / "elsewhere.db");
}

// --- Migrations ------------------------------------------------------------------

TEST_CASE("a fresh database opens at schema version 1 in WAL mode", "[session][migrations]") {
    TempDir dir;
    const auto path = dir.path() / "fresh.db";

    {
        SessionDatabase db(path);
        REQUIRE(db.ListSessions({}).empty());
    }

    CHECK(RawScalar(path, "PRAGMA user_version;") == "1");
    CHECK(RawScalar(path, "PRAGMA journal_mode;") == "wal");
    const auto tables = RawTableNames(path);
    CHECK(tables.count("sessions") == 1);
    CHECK(tables.count("messages") == 1);
}

TEST_CASE("a database newer than the binary is refused with a clear error", "[session][migrations]") {
    TempDir dir;
    const auto path = dir.path() / "new.db";

    // Hand-craft a database claiming schema version 99.
    sqlite3* raw = RawOpen(path);
    REQUIRE(sqlite3_exec(raw, "PRAGMA user_version = 99;", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    try {
        SessionDatabase db(path);
        FAIL("opening a newer database should have thrown");
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        CHECK(what.find("newer") != std::string::npos);
        CHECK(what.find("99") != std::string::npos);
    }
}

TEST_CASE("a failing migration rolls back completely", "[session][migrations]") {
    TempDir dir;
    const auto path = dir.path() / "mig.db";

    // Fault-injected migration list: the first statement succeeds, the second
    // does not. If migrations were not transactional, `leftover` would survive.
    OpenOptions bad;
    bad.migrations = {{1, {"CREATE TABLE leftover (x INTEGER);", "THIS IS NOT SQL;"}}};
    REQUIRE_THROWS_AS([&] { SessionDatabase db(path, bad); }(), std::runtime_error);

    const auto tables = RawTableNames(path);
    CHECK(tables.count("leftover") == 0);
    CHECK(tables.count("sessions") == 0);
    CHECK(tables.count("messages") == 0);

    // The database is still at version 0, so a clean reopen applies the real
    // schema from scratch.
    SessionDatabase db(path);
    REQUIRE(db.ListSessions({}).empty());
    CHECK(RawScalar(path, "PRAGMA user_version;") == "1");
}

// --- Write path -------------------------------------------------------------------

TEST_CASE("appends persist and reload structurally identical", "[session][store]") {
    TempDir dir;
    const auto path = dir.path() / "roundtrip.db";

    std::string session_id;
    {
        SessionDatabase db(path);
        const auto session = db.CreateSession("", std::nullopt, "/proj", "build");
        session_id = session.session_id;
        SqliteHistoryStore store(db, session_id);

        // A full tool cycle: user text, assistant tool use, tool result, final text.
        store.Append(UserMsg("read the file"), std::nullopt);
        llm::Message tool_use{llm::Role::Assistant, {}};
        tool_use.content.push_back(llm::ToolUseBlock{"toolu_1", "read", {{"path", "a.cpp"}}});
        store.Append(tool_use, std::nullopt);
        llm::ToolResultBlock result;
        result.tool_use_id = "toolu_1";
        result.content.push_back(llm::TextBlock{"int main() {}"});
        store.Append(llm::Message{llm::Role::User, {result}}, std::nullopt);
        store.Append(llm::Message{llm::Role::Assistant, {llm::TextBlock{"it is a trivial program"}}}, 42);
    }

    {
        SessionDatabase db(path);
        SessionReader reader(db);
        const auto page = reader.GetMessages(session_id, std::nullopt, 100);
        REQUIRE(page.messages.size() == 4);
        CHECK_FALSE(page.has_more);

        // Descending by seq: newest first.
        REQUIRE(page.messages[3].seq == 0);
        REQUIRE(page.messages[2].seq == 1);
        REQUIRE(page.messages[1].seq == 2);
        REQUIRE(page.messages[0].seq == 3);

        CHECK(MessagesEqual(page.messages[3].message, UserMsg("read the file")));
        CHECK(std::get<llm::ToolUseBlock>(page.messages[2].message.content[0]).input.at("path") == "a.cpp");
        const auto* reloaded_result = std::get_if<llm::ToolResultBlock>(&page.messages[1].message.content[0]);
        REQUIRE(reloaded_result != nullptr);
        CHECK(reloaded_result->tool_use_id == "toolu_1");
        CHECK(page.messages[0].message.content.size() == 1);
        // Assistant rows carry the reported output count; user rows do not.
        CHECK(page.messages[0].token_count == 42);
        CHECK(page.messages[3].token_count == 0);
    }
}

TEST_CASE("seqs are per-session and independent", "[session][store]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "two.db");
    const auto a = db.CreateSession("", std::nullopt, "/a", "build").session_id;
    const auto b = db.CreateSession("", std::nullopt, "/b", "build").session_id;

    db.AppendMessage(a, UserMsg("a1"));
    db.AppendMessage(b, UserMsg("b1"));
    db.AppendMessage(a, UserMsg("a2"));

    const auto page_a = db.GetMessagesPage(a, std::nullopt, 10);
    const auto page_b = db.GetMessagesPage(b, std::nullopt, 10);
    REQUIRE(page_a.messages.size() == 2);
    REQUIRE(page_b.messages.size() == 1);
    CHECK(page_a.messages[0].seq == 1);
    CHECK(page_a.messages[1].seq == 0);
    CHECK(page_b.messages[0].seq == 0);
}

TEST_CASE("appending to an unknown session is an error, not a dropped row", "[session][store]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "unknown.db");
    CHECK_THROWS_AS(db.AppendMessage("no-such-session", UserMsg("hi")), std::runtime_error);
}

TEST_CASE("creating a session with an unknown parent is an error", "[session][store]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "parent.db");
    CHECK_THROWS_AS(
        (db.CreateSession("", std::string("no-such-parent"), "/p", "build")), std::runtime_error);
}

// --- Default session naming --------------------------------------------------------

TEST_CASE("the first user message names an unnamed session, once", "[session][naming]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "naming.db");
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;

    db.AppendMessage(id, UserMsg("Fix the login bug on the auth service"));
    CHECK(db.GetSession(id)->name == "Fix the login bug on the auth service");

    // Later messages never clobber the name.
    db.AppendMessage(id, UserMsg("now also handle the logout path please"));
    CHECK(db.GetSession(id)->name == "Fix the login bug on the auth service");
}

TEST_CASE("an explicit creation name is preserved", "[session][naming]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "explicit.db");
    const auto id = db.CreateSession("my chosen name", std::nullopt, "/p", "build").session_id;
    db.AppendMessage(id, UserMsg("hello there"));
    CHECK(db.GetSession(id)->name == "my chosen name");
}

TEST_CASE("a tool-result-only user message is not a name", "[session][naming]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "toolonly.db");
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;

    llm::ToolResultBlock result;
    result.tool_use_id = "t1";
    result.content.push_back(llm::TextBlock{"subagent finished its work"});
    db.AppendMessage(id, llm::Message{llm::Role::User, {result}});
    CHECK(db.GetSession(id)->name.empty());

    // The next user message with text still names the session.
    db.AppendMessage(id, UserMsg("the real question"));
    CHECK(db.GetSession(id)->name == "the real question");
}

TEST_CASE("names are whitespace-collapsed and truncated at 60 code points", "[session][naming]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "trunc.db");

    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    db.AppendMessage(id, UserMsg("  Fix   the\nlogin\tbug  "));
    CHECK(db.GetSession(id)->name == "Fix the login bug");

    // ASCII: exactly 60 characters survive.
    const auto id2 = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    db.AppendMessage(id2, UserMsg(std::string(70, 'a')));
    CHECK(db.GetSession(id2)->name == std::string(60, 'a'));

    // UTF-8: truncation counts code points, never splitting a sequence.
    const auto id3 = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    std::string long_unicode;
    for (int i = 0; i < 70; ++i) long_unicode += "\xC3\xA9";  // é × 70
    db.AppendMessage(id3, UserMsg(long_unicode));
    CHECK(db.GetSession(id3)->name == long_unicode.substr(0, 120));  // 60 × 2 bytes
}

TEST_CASE("UpdateSessionName renames and reports unknown ids", "[session][naming]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "rename.db");
    const auto id = db.CreateSession("old", std::nullopt, "/p", "build").session_id;
    CHECK(db.UpdateSessionName(id, "new"));
    CHECK(db.GetSession(id)->name == "new");
    CHECK_FALSE(db.UpdateSessionName("no-such-session", "x"));
}

// --- Crash safety ---------------------------------------------------------------------

TEST_CASE("a SIGKILL mid-append leaves an exact committed prefix", "[session][crash]") {
    TempDir dir;
    const auto path = dir.path() / "crash.db";

    // The child opens its own connection and never touches parent state: this
    // test holds no SessionDatabase across the fork.
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        try {
            SessionDatabase db(path);
            const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
            // A guaranteed-committed warm-up batch, then a slow endless loop the
            // parent will kill mid-transaction or between transactions.
            for (int i = 0; i < 20; ++i) {
                db.AppendMessage(id, UserMsg("warmup " + std::to_string(i)));
            }
            for (int i = 0;; ++i) {
                db.AppendMessage(id, UserMsg("loop " + std::to_string(i)));
                usleep(200);
            }
        } catch (...) {
            // Setup failure in the child: report via exit status.
        }
        _exit(42);  // unreachable while the loop runs
    }

    usleep(300 * 1000);
    REQUIRE(kill(child, SIGKILL) == 0);
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFSIGNALED(status));
    REQUIRE(WTERMSIG(status) == SIGKILL);

    // Reopen: the WAL recovers, the transcript is an exact prefix, and no row is
    // torn -- strict decoding of every page would throw on a partial write.
    SessionDatabase db(path);
    const auto sessions = db.ListSessions({.include_subagents = true});
    REQUIRE(sessions.size() == 1);
    const int count = sessions[0].message_count;
    REQUIRE(count >= 20);

    SessionReader reader(db);
    std::set<int64_t> seen_seqs;
    std::optional<int64_t> before_seq;  // no cursor: start at the most recent
    while (true) {
        const auto page = reader.GetMessages(sessions[0].session_id, before_seq, 100);
        REQUIRE(!page.messages.empty());
        for (const auto& stored : page.messages) seen_seqs.insert(stored.seq);
        if (!page.has_more) break;
        before_seq = page.messages.back().seq;
    }
    REQUIRE(seen_seqs.size() == static_cast<std::size_t>(count));
    for (int64_t seq = 0; seq < count; ++seq) {
        CHECK(seen_seqs.count(seq) == 1);
    }

    CHECK(RawScalar(path, "PRAGMA integrity_check;") == "ok");
}

// --- Read path: keyset paging -----------------------------------------------------------

TEST_CASE("keyset paging walks a large transcript in exact order", "[session][paging]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "paging.db");
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;

    constexpr int kMessages = 5000;
    for (int i = 0; i < kMessages; ++i) {
        db.AppendMessage(id, UserMsg("m" + std::to_string(i)));
    }

    SessionReader reader(db);

    // First page: the 100 most recent, descending.
    const auto first = reader.GetMessages(id, std::nullopt, 100);
    REQUIRE(first.messages.size() == 100);
    CHECK(first.has_more);
    CHECK(first.messages[0].seq == kMessages - 1);
    CHECK(first.messages[99].seq == kMessages - 100);

    // Second page starts strictly before the last seq of the first.
    const auto second = reader.GetMessages(id, first.messages.back().seq, 100);
    REQUIRE(second.messages.size() == 100);
    CHECK(second.has_more);
    CHECK(second.messages[0].seq == kMessages - 101);

    // Walk the whole transcript; every seq exactly once, in order.
    std::vector<int64_t> all_seqs;
    std::optional<int64_t> before_seq;  // no cursor: start at the most recent
    while (true) {
        const auto page = reader.GetMessages(id, before_seq, 100);
        for (const auto& stored : page.messages) all_seqs.push_back(stored.seq);
        if (!page.has_more) break;
        before_seq = page.messages.back().seq;
    }
    REQUIRE(all_seqs.size() == static_cast<std::size_t>(kMessages));
    for (int i = 0; i < kMessages; ++i) {
        CHECK(all_seqs[i] == kMessages - 1 - i);
    }

    // A limit of one still reports has_more correctly.
    const auto single = reader.GetMessages(id, std::nullopt, 1);
    REQUIRE(single.messages.size() == 1);
    CHECK(single.has_more);
    CHECK(single.messages[0].seq == kMessages - 1);

    // Paging an empty session yields an empty page, not an error.
    const auto empty_id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    const auto empty = reader.GetMessages(empty_id, std::nullopt, 10);
    REQUIRE(empty.messages.empty());
    CHECK_FALSE(empty.has_more);

    // A non-positive limit is a caller bug.
    CHECK_THROWS_AS(reader.GetMessages(id, std::nullopt, 0), std::runtime_error);
}

TEST_CASE("ListSessions pages, and the cursor survives a same-millisecond tie",
          "[session][listing]") {
    // Unbounded, this returns every session ever created, each with its own
    // COUNT(*) over the messages table. The cursor is a (updated_at, id) pair
    // because the sort key is: NextTimestamp only guarantees monotonicity within
    // a process, so a database written across restarts can hold ties, and a
    // cursor on updated_at alone would skip or repeat rows at a page boundary.
    TempDir dir;
    SessionDatabase db(dir.path() / "s.db");
    for (int i = 0; i < 7; ++i) {
        db.CreateSession("s" + std::to_string(i), std::nullopt, "/p", "build");
    }

    std::vector<std::string> paged;
    std::optional<std::int64_t> cursor_ts;
    std::optional<std::string> cursor_id;
    while (true) {
        ListSessionsFilter filter;
        filter.include_subagents = true;
        filter.limit = 3;
        filter.before_updated_at = cursor_ts;
        filter.before_id = cursor_id;
        const auto page = db.ListSessions(filter);
        if (page.empty()) break;
        CHECK(page.size() <= 3);
        for (const auto& s : page) paged.push_back(s.session_id);
        cursor_ts = page.back().updated_at_unix_ms;
        cursor_id = page.back().session_id;
    }

    const auto all = db.ListSessions({.include_subagents = true});
    REQUIRE(all.size() == 7);
    REQUIRE(paged.size() == all.size());  // no row skipped, none repeated
    for (std::size_t i = 0; i < all.size(); ++i) {
        CHECK(paged[i] == all[i].session_id);
    }
}

TEST_CASE("SessionExists answers without counting the transcript", "[session][listing]") {
    // GetSession carries a COUNT(*) over every message, which GetMessages threw
    // away on every page -- an O(n) count in front of the keyset seek whose whole
    // purpose is that a page costs the same on a long transcript as a short one.
    TempDir dir;
    SessionDatabase db(dir.path() / "s.db");
    const auto id = db.CreateSession("s", std::nullopt, "/p", "build").session_id;
    for (int i = 0; i < 50; ++i) db.AppendMessage(id, UserMsg("m"), 0);

    CHECK(db.SessionExists(id));
    CHECK_FALSE(db.SessionExists("no-such-session"));
    // The counting version still counts, for the callers that want the number.
    REQUIRE(db.GetSession(id).has_value());
    CHECK(db.GetSession(id)->message_count == 50);
}

TEST_CASE("a cursor of 0 pages before the first message, not from the newest",
          "[session][paging][invariant]") {
    // seq starts at 0, so the old `before_seq <= 0 means no cursor` sentinel was
    // drawn from the value space it had to be distinguishable from. A client
    // that paged back to the first message and passed its seq as the next cursor
    // got the NEWEST page again -- the read equivalent of a loop that never
    // terminates. Absence is now carried by the optional, not by a value.
    TempDir dir;
    SessionDatabase db(dir.path() / "s.db");
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    for (int i = 0; i < 3; ++i) db.AppendMessage(id, UserMsg("m" + std::to_string(i)), 0);

    SessionReader reader(db);
    const std::int64_t first_seq = 0;

    // The cursor case: strictly before seq 0 is nothing at all.
    const auto before_first = reader.GetMessages(id, first_seq, 10);
    CHECK(before_first.messages.empty());
    CHECK_FALSE(before_first.has_more);

    // The no-cursor case, from the same call, differing only in the optional.
    const auto newest = reader.GetMessages(id, std::nullopt, 10);
    REQUIRE(newest.messages.size() == 3);
    CHECK(newest.messages.front().seq == 2);
}

// --- Session listing -----------------------------------------------------------------------

TEST_CASE("ListSessions filters by project and hides subagents by default", "[session][listing]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "list.db");

    const auto a1 = db.CreateSession("", std::nullopt, "/proj/a", "build").session_id;
    db.AppendMessage(a1, UserMsg("a1 message"));
    const auto child = db.CreateSession("", a1, "/proj/a", "subagent").session_id;
    db.AppendMessage(child, UserMsg("child work"));
    const auto a2 = db.CreateSession("", std::nullopt, "/proj/a", "build").session_id;
    db.AppendMessage(a2, UserMsg("a2 message"));
    const auto b1 = db.CreateSession("", std::nullopt, "/proj/b", "build").session_id;

    // Default: one project, no subagents. a2 is most recently active, so first.
    std::vector<std::string> ids;
    for (const auto& s : db.ListSessions({.project_root = "/proj/a", .include_subagents = false})) ids.push_back(s.session_id);
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == a2);
    CHECK(ids[1] == a1);

    // include_subagents adds the child.
    ids.clear();
    for (const auto& s : db.ListSessions({.project_root = "/proj/a", .include_subagents = true})) ids.push_back(s.session_id);
    REQUIRE(ids.size() == 3);
    CHECK(std::find(ids.begin(), ids.end(), child) != ids.end());

    // include_all_dirs: every root session, still no subagents.
    ids.clear();
    for (const auto& s : db.ListSessions({.include_subagents = false})) ids.push_back(s.session_id);
    REQUIRE(ids.size() == 3);
    CHECK(std::find(ids.begin(), ids.end(), child) == ids.end());
    CHECK(std::find(ids.begin(), ids.end(), b1) != ids.end());

    // message_count is accurate per row.
    for (const auto& s : db.ListSessions({.include_subagents = true})) {
        if (s.session_id == a1) CHECK(s.message_count == 1);
        if (s.session_id == child) CHECK(s.message_count == 1);
        if (s.session_id == b1) CHECK(s.message_count == 0);
    }
}

TEST_CASE("ListSessions sorts by last activity, newest first", "[session][listing]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "sort.db");

    const auto older = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    db.AppendMessage(older, UserMsg("first"));
    const auto newer = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    db.AppendMessage(newer, UserMsg("second"));

    // Touch the older session so it becomes most recently active.
    db.AppendMessage(older, UserMsg("third"));

    const auto sessions = db.ListSessions({.include_subagents = false});
    REQUIRE(sessions.size() == 2);
    CHECK(sessions[0].session_id == older);
    CHECK(sessions[1].session_id == newer);
}

// --- Deletion -------------------------------------------------------------------------------

TEST_CASE("DeleteSession removes the session, its messages, and its children", "[session][delete]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "delete.db");

    const auto parent = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    db.AppendMessage(parent, UserMsg("parent message"));
    const auto child = db.CreateSession("", parent, "/p", "subagent").session_id;
    db.AppendMessage(child, UserMsg("child message"));

    REQUIRE(db.DeleteSession(parent));
    CHECK_FALSE(db.DeleteSession(parent));  // already gone

    CHECK(db.GetSession(parent).has_value() == false);
    CHECK(db.GetSession(child).has_value() == false);  // cascade through the FK
    const auto page = db.GetMessagesPage(parent, std::nullopt, 10);
    REQUIRE(page.messages.empty());
    CHECK(db.ListSessions({.include_subagents = true}).empty());
}

// --- Concurrency ------------------------------------------------------------------------------

TEST_CASE("concurrent appends across sessions stay consistent", "[session][concurrency]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "conc.db");

    constexpr int kSessions = 4;
    constexpr int kThreads = 4;
    constexpr int kAppendsPerThread = 250;

    std::vector<std::string> ids(kSessions);
    for (int i = 0; i < kSessions; ++i) {
        ids[i] = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kAppendsPerThread; ++i) {
                // Round-robin across sessions: every thread writes to every session,
                // so the shared connection sees maximum interleaving.
                const auto& id = ids[(t + i) % kSessions];
                db.AppendMessage(id, UserMsg("w" + std::to_string(t) + ":" + std::to_string(i)));
            }
        });
    }
    for (auto& thread : threads) thread.join();

    // Each session received exactly kThreads * kAppendsPerThread / kSessions rows,
    // with contiguous seqs -- the single-writer discipline made visible.
    const int expected = kThreads * kAppendsPerThread / kSessions;
    for (const auto& id : ids) {
        const auto page = db.GetMessagesPage(id, std::nullopt, expected + 1);
        REQUIRE(page.messages.size() == static_cast<std::size_t>(expected));
        CHECK_FALSE(page.has_more);
        for (int i = 0; i < expected; ++i) {
            CHECK(page.messages[i].seq == expected - 1 - i);
        }
    }
}
