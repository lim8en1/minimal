#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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
                          {{"command", "make 2>&1 | tail -50"}, {"timeout", 30}}});
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

TEST_CASE("codec round-trips the multi-block message @-mention expansion produces",
          "[session][codec][mention]") {
    // Verbatim text block + one framed attachment block per mention -- the shape
    // ExpandMentions returns and scrollback must be able to render separately.
    const llm::Message message{
        llm::Role::User,
        {llm::TextBlock{"see @a.md and @b.md"},
         llm::TextBlock{"<file path=\"a.md\">\nA\n</file>"},
         llm::TextBlock{"<file path=\"b.md\">\nB\n</file>"}}};
    CHECK(MessagesEqual(RoundTrip(message), message));
}

TEST_CASE("codec degrades an unknown block type to a readable placeholder",
          "[session][codec][invariant]") {
    // The upgrade-hazard decision: one unknown block type in a stored
    // transcript used to make every row it was in unreadable -- bricking
    // scrollback AND resume for an older binary. The placeholder is the
    // warning, visible to client and model.
    nlohmann::json doc = {{"role", "user"},
                          {"content", {{{"type", "warp_drive"}, {"text", "hi"}}}}};
    const auto message = JsonToMessage(doc);
    REQUIRE(message.content.size() == 1);
    const auto* text = std::get_if<llm::TextBlock>(&message.content[0]);
    REQUIRE(text != nullptr);
    CHECK(text->text.find("unknown block type \"warp_drive\"") != std::string::npos);
    CHECK(text->text.find("newer version") != std::string::npos);
}

TEST_CASE("a transcript with a block from a newer version scrolls and resumes",
          "[session][codec][invariant]") {
    // The E2E half of the placeholder decision: a stored row containing a
    // block type this binary does not know must not brick scrollback or
    // resume. Written here the way a newer binary would have written it
    // (raw sqlite, unknown type), read the way an older binary reads.
    TempDir dir;
    const auto path = dir.path() / "forward.db";
    std::string id;
    {
        SessionDatabase db(path);
        id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
        db.AppendMessage(id, UserMsg("older text"));
    }
    {
        sqlite3* raw = RawOpen(path);
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(raw,
                                   "INSERT INTO messages (session_id, seq, role,"
                                   " content_json, token_count, created_at)"
                                   " VALUES (?1, 1, 'user', ?2, 0, 0);",
                                   -1, &stmt, nullptr) == SQLITE_OK);
        sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        const std::string json =
            R"({"role":"user","content":[)"
            R"({"type":"hologram","data":{"x":1}},{"type":"text","text":"tail"}]})";
        sqlite3_bind_text(stmt, 2, json.c_str(), -1, SQLITE_TRANSIENT);
        REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        sqlite3_close(raw);
    }

    SessionDatabase older(path);

    // Scrollback: pages fine, the unknown block is the placeholder, the
    // sibling text block survives verbatim.
    const auto page = older.GetMessagesPage(id, std::nullopt, 10);
    REQUIRE(page.messages.size() == 2);
    const auto& newer_row = page.messages[0].message.content;  // seq 1, descending
    REQUIRE(newer_row.size() == 2);
    const auto* placeholder = std::get_if<llm::TextBlock>(&newer_row[0]);
    REQUIRE(placeholder != nullptr);
    CHECK(placeholder->text.find("unknown block type \"hologram\"") != std::string::npos);
    CHECK(std::get<llm::TextBlock>(newer_row[1]).text == "tail");

    // Resume: the full load is equally unaffected.
    const auto all = SessionReader(older).LoadAll(id);
    REQUIRE(all.size() == 2);
    CHECK(all[1].message.content.size() == 2);
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
    // Tool result content present but NOT an array -- this used to be
    // silently dropped (empty content, no error), the exact failure the
    // strict contract exists to prevent.
    nlohmann::json non_array = {{"role", "user"},
                                {"content",
                                 {{{"type", "tool_result"},
                                   {"tool_use_id", "t"},
                                   {"content", "plain text"}}}}};
    CHECK_THROWS_AS(JsonToMessage(non_array), std::runtime_error);
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

TEST_CASE("a fresh database opens at the current schema version in WAL mode",
          "[session][migrations]") {
    TempDir dir;
    const auto path = dir.path() / "fresh.db";

    {
        SessionDatabase db(path);
        REQUIRE(db.ListSessions({}).empty());
        // Per-connection, hence an accessor rather than a RawScalar: a fresh
        // connection reports SQLite's default, not this one's setting. 0 is
        // the invariant -- the write path never self-checkpoints (the reaper
        // owns it).
        CHECK(db.wal_autocheckpoint() == 0);
    }

    CHECK(RawScalar(path, "PRAGMA user_version;") ==
          std::to_string(SessionDatabase::kSchemaVersion));
    CHECK(RawScalar(path, "PRAGMA journal_mode;") == "wal");
    const auto tables = RawTableNames(path);
    CHECK(tables.count("sessions") == 1);
    CHECK(tables.count("messages") == 1);
}

TEST_CASE("the default migration list ends at kSchemaVersion", "[session][migrations][invariant]") {
    // The guard: kSchemaVersion is the TARGET the version arithmetic stamps,
    // so it must name the last migration's version. A migration added without
    // the bump (or a bump without its migration) would otherwise advance
    // user_version past statements that never ran -- re-applied or skipped
    // forever, silently. The guard in DefaultMigrations makes that a loud
    // startup failure; this asserts the property directly.
    const auto& migrations = DefaultMigrations();
    REQUIRE(!migrations.empty());
    CHECK(migrations.back().version == SessionDatabase::kSchemaVersion);
    int expected_version = 1;
    for (const auto& migration : migrations) {
        CHECK(migration.version == expected_version++);
    }
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
    CHECK(RawScalar(path, "PRAGMA user_version;") ==
          std::to_string(SessionDatabase::kSchemaVersion));
}

TEST_CASE("migration to the message_count column backfills from existing rows",
          "[session][migrations]") {
    // A v1 database holds rows the counter column knows nothing about. The
    // backfill runs inside the migration transaction; without it the picker
    // would show 0 messages for every pre-v2 session. The rows are inserted
    // raw: the binary's append SQL writes the v2 column, so it cannot run
    // against a v1 schema -- that is what the migration is for.
    TempDir dir;
    const auto path = dir.path() / "upgrade.db";

    // Schema at v1 ONLY, hand-crafted. (OpenOptions' migration list is a
    // fault-injection seam, not a version simulator: ApplyMigrations stamps the
    // target version, so an injected v1 list still yields user_version = 2.)
    {
        sqlite3* raw = RawOpen(path);
        REQUIRE(sqlite3_exec(raw,
                             "CREATE TABLE sessions ("
                             "  id TEXT PRIMARY KEY,"
                             "  parent_session_id TEXT REFERENCES sessions(id) ON DELETE CASCADE,"
                             "  name TEXT NOT NULL DEFAULT '',"
                             "  project_root TEXT NOT NULL,"
                             "  agent_preset TEXT NOT NULL,"
                             "  created_at INTEGER NOT NULL,"
                             "  updated_at INTEGER NOT NULL"
                             ");",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_exec(raw,
                             "CREATE TABLE messages ("
                             "  id INTEGER PRIMARY KEY,"
                             "  session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
                             "  seq INTEGER NOT NULL,"
                             "  role TEXT NOT NULL,"
                             "  content_json TEXT NOT NULL,"
                             "  token_count INTEGER NOT NULL DEFAULT 0,"
                             "  created_at INTEGER NOT NULL,"
                             "  UNIQUE (session_id, seq)"
                             ");",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_exec(raw, "PRAGMA user_version = 1;", nullptr, nullptr, nullptr) ==
                SQLITE_OK);
        sqlite3_close(raw);
    }

    const std::string session_id = GenerateUlid();
    {
        sqlite3* raw = RawOpen(path);
        const std::int64_t now = 1700000000000;
        char sql[512];
        std::snprintf(sql, sizeof(sql),
                      "INSERT INTO sessions (id, name, project_root, agent_preset, created_at,"
                      " updated_at) VALUES ('%s', 's', '/p', 'build', %lld, %lld);",
                      session_id.c_str(), static_cast<long long>(now),
                      static_cast<long long>(now));
        REQUIRE(sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
        for (int i = 0; i < 3; ++i) {
            const std::string json = MessageToJson(UserMsg("m" + std::to_string(i))).dump();
            std::snprintf(sql, sizeof(sql),
                          "INSERT INTO messages (session_id, seq, role, content_json, token_count,"
                          " created_at) VALUES ('%s', %d, 'user', '%s', 0, %lld);",
                          session_id.c_str(), i, json.c_str(), static_cast<long long>(now));
            REQUIRE(sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
        }
        sqlite3_close(raw);
    }
    REQUIRE(RawScalar(path, "PRAGMA user_version;") == "1");

    // Reopen at the current version: the migration applies and backfills.
    SessionDatabase db(path);
    REQUIRE(RawScalar(path, "PRAGMA user_version;") ==
            std::to_string(SessionDatabase::kSchemaVersion));
    const auto sessions = db.ListSessions({});
    REQUIRE(sessions.size() == 1);
    CHECK(sessions[0].message_count == 3);
}

TEST_CASE("message_count stays exact across appends and restarts", "[session][migrations]") {
    TempDir dir;
    const auto path = dir.path() / "s.db";
    {
        SessionDatabase db(path);
        const auto id = db.CreateSession("s", std::nullopt, "/p", "build").session_id;
        for (int i = 0; i < 7; ++i) db.AppendMessage(id, UserMsg("m"), 0);
        REQUIRE(db.GetSession(id).has_value());
        CHECK(db.GetSession(id)->message_count == 7);
    }
    // Reopened, the count is read back from the column, not recomputed -- so a
    // drifted column would show up as a wrong number here rather than silently
    // healing. It must not drift: the bump lives in the append transaction.
    SessionDatabase db(path);
    const auto sessions = db.ListSessions({});
    REQUIRE(sessions.size() == 1);
    CHECK(sessions[0].message_count == 7);
}

TEST_CASE("migration to name_source upgrades a v4 database in place", "[session][migrations]") {
    // A v4 database predates name_source. The ALTER adds the column with ''
    // -- "not known to be explicit", the safe reading for pre-v6 rows -- and
    // the store keeps working over it.
    TempDir dir;
    const auto path = dir.path() / "v4.db";

    {
        sqlite3* raw = RawOpen(path);
        REQUIRE(sqlite3_exec(raw,
                             "CREATE TABLE sessions ("
                             "  id TEXT PRIMARY KEY,"
                             "  parent_session_id TEXT REFERENCES sessions(id) ON DELETE CASCADE,"
                             "  name TEXT NOT NULL DEFAULT '',"
                             "  project_root TEXT NOT NULL,"
                             "  agent_preset TEXT NOT NULL,"
                             "  created_at INTEGER NOT NULL,"
                             "  updated_at INTEGER NOT NULL,"
                             "  message_count INTEGER NOT NULL DEFAULT 0"
                             ");",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_exec(raw,
                             "CREATE TABLE messages ("
                             "  id INTEGER PRIMARY KEY,"
                             "  session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
                             "  seq INTEGER NOT NULL,"
                             "  role TEXT NOT NULL,"
                             "  content_json TEXT NOT NULL,"
                             "  token_count INTEGER NOT NULL DEFAULT 0,"
                             "  created_at INTEGER NOT NULL,"
                             "  UNIQUE (session_id, seq)"
                             ");",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_exec(raw,
                             "CREATE TABLE compactions ("
                             "  session_id TEXT PRIMARY KEY REFERENCES sessions(id) ON DELETE"
                             " CASCADE,"
                             "  boundary_seq INTEGER NOT NULL,"
                             "  summary_content_json TEXT NOT NULL,"
                             "  created_at INTEGER NOT NULL"
                             ");",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_exec(raw, "PRAGMA user_version = 4;", nullptr, nullptr, nullptr) ==
                SQLITE_OK);
        const std::string session_id = GenerateUlid();
        char sql[512];
        std::snprintf(sql, sizeof(sql),
                      "INSERT INTO sessions (id, name, project_root, agent_preset, created_at,"
                      " updated_at) VALUES ('%s', 'old name', '/p', 'build', 1, 1);",
                      session_id.c_str());
        REQUIRE(sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(raw);
    }
    REQUIRE(RawScalar(path, "PRAGMA user_version;") == "4");

    SessionDatabase db(path);
    REQUIRE(RawScalar(path, "PRAGMA user_version;") ==
            std::to_string(SessionDatabase::kSchemaVersion));
    const auto sessions = db.ListSessions({});
    REQUIRE(sessions.size() == 1);
    // Backfilled rows read as "not known to be explicit" -- the safe reading,
    // since a pre-v6 session is old and the titler only ever fires for a fresh
    // session's first turn in its creating process.
    CHECK(sessions[0].name_source.empty());
    CHECK_FALSE(db.IsExplicitlyNamed(sessions[0].session_id));
    // The store keeps working over the upgraded schema.
    CHECK(db.UpdateSessionName(sessions[0].session_id, "renamed post-upgrade"));
    CHECK(db.GetSession(sessions[0].session_id)->name_source == "observer");
}

TEST_CASE("paging constrains on seq -- the planner seeks, it does not walk",
          "[session][paging][invariant]") {
    // THE INVARIANT behind "a page costs the same on a long transcript": the
    // production predicate binds COALESCE(?, INT64_MAX) so the planner applies
    // the seq range constraint. The previous `?2 IS NULL OR seq < ?2` shape
    // planned as equality on session_id plus a plain index walk from MAX(seq)
    // -- O(n) per page, O(n^2) to page a whole transcript. This is EXPLAIN'd
    // against the PRODUCTION STATEMENT (kMessagesPageSql, not a test-local
    // transcription -- a lookalike drifts silently when the statement
    // changes) with a bound cursor; the reverted shape drops the constraint
    // under the same EXPLAIN, which is what made the fix necessary.
    TempDir dir;
    SessionDatabase db(dir.path() / "s.db");
    const auto id = db.CreateSession("s", std::nullopt, "/p", "build").session_id;
    for (int i = 0; i < 10; ++i) db.AppendMessage(id, UserMsg("m"), 0);
    (void)id;

    sqlite3* raw = RawOpen(dir.path() / "s.db");
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw,
                               ("EXPLAIN QUERY PLAN " + std::string(kMessagesPageSql)).c_str(),
                               -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "s", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, 5);

    std::string plan;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        plan += reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        plan += "; ";
    }
    sqlite3_finalize(stmt);
    sqlite3_close(raw);

    // The plan must constrain on seq -- a cursor page must be a seek, not a
    // walk from MAX(seq) past every newer row.
    CHECK(plan.find("seq<?") != std::string::npos);
}

TEST_CASE("the session picker seeks on its cursor -- the planner constrains, it does not scan",
          "[session][listing][invariant]") {
    // The un-swept sibling of the messages fix above: ListSessions kept the
    // `?3 IS NULL OR ...` prefix, which planned as SCAN over
    // idx_sessions_activity -- every page request re-walked the index from the
    // top past every newer session, O(n) per page and O(n^2) to page the
    // picker. The ceiling is now `updated_at <= COALESCE(?3, INT64_MAX)`,
    // which the planner constrains on. EXPLAIN against the PRODUCTION
    // statement (kSessionsPageSql), mirroring the messages invariant.
    TempDir dir;
    SessionDatabase db(dir.path() / "s.db");
    std::string cursor_id;
    std::int64_t cursor_updated_at = 0;
    for (int i = 0; i < 10; ++i) {
        const auto created = db.CreateSession("", std::nullopt, "/p", "build");
        if (i == 4) {
            cursor_id = created.session_id;
            cursor_updated_at = created.updated_at_unix_ms;
        }
    }

    sqlite3* raw = RawOpen(dir.path() / "s.db");
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw, ("EXPLAIN QUERY PLAN " + std::string(kSessionsPageSql)).c_str(),
                               -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_null(stmt, 1);              // all project roots
    sqlite3_bind_int(stmt, 2, 0);            // hide subagents
    sqlite3_bind_int64(stmt, 3, cursor_updated_at);
    sqlite3_bind_text(stmt, 4, cursor_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, 5);

    std::string plan;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        plan += reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        plan += "; ";
    }
    sqlite3_finalize(stmt);
    sqlite3_close(raw);

    // A SEARCH constrained on updated_at -- not a SCAN from the top of the
    // index past every newer session.
    CHECK(plan.find("SEARCH") != std::string::npos);
    CHECK(plan.find("updated_at<?") != std::string::npos);
}

TEST_CASE("paging the picker over a thousand sessions is exact", "[session][listing]") {
    // The functional half of the cursor fix: page size must be the ONLY cost
    // driver, and the pages must tile the session list exactly -- every
    // session exactly once, in (updated_at DESC, id ASC) order, with the
    // boundary between pages exact. The EXPLAIN invariant above proves the
    // seek; this proves the seek did not change what is returned.
    TempDir dir;
    SessionDatabase db(dir.path() / "many.db");
    for (int i = 0; i < 1000; ++i) {
        (void)db.CreateSession("", std::nullopt, "/p", "build");
    }

    std::vector<SessionSummary> seen;
    std::optional<std::int64_t> before_updated_at;
    std::optional<std::string> before_id;
    while (true) {
        const auto page = db.ListSessions(
            {std::nullopt, false, 100, before_updated_at, before_id});
        REQUIRE(page.size() <= 100);
        if (page.empty()) break;
        REQUIRE((seen.empty() || seen.back().updated_at_unix_ms >= page.front().updated_at_unix_ms));        for (const auto& session : page) seen.push_back(session);
        before_updated_at = page.back().updated_at_unix_ms;
        before_id = page.back().session_id;
    }

    REQUIRE(seen.size() == 1000);
    for (std::size_t i = 1; i < seen.size(); ++i) {
        CHECK(seen[i - 1].updated_at_unix_ms >= seen[i].updated_at_unix_ms);
        if (seen[i - 1].updated_at_unix_ms == seen[i].updated_at_unix_ms) {
            CHECK(seen[i - 1].session_id < seen[i].session_id);  // ties: id ASC
        }
    }
    std::unordered_set<std::string> unique;
    for (const auto& session : seen) unique.insert(session.session_id);
    CHECK(unique.size() == 1000);  // no session repeated or skipped
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

// --- Resume: load-without-appending and seq continuation ---------------------

TEST_CASE("LoadMessages returns the whole transcript ascending", "[session][resume]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "load.db");
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;

    llm::Message tool_use{llm::Role::Assistant, {}};
    tool_use.content.push_back(llm::ToolUseBlock{"toolu_1", "read", {{"path", "a.cpp"}}});
    llm::ToolResultBlock result;
    result.tool_use_id = "toolu_1";
    result.content.push_back(llm::TextBlock{"contents"});

    db.AppendMessage(id, UserMsg("first"));
    db.AppendMessage(id, tool_use);
    db.AppendMessage(id, llm::Message{llm::Role::User, {result}});
    db.AppendMessage(id, llm::Message{llm::Role::Assistant, {llm::TextBlock{"last"}}}, 17);

    SessionReader reader(db);
    const auto loaded = reader.LoadAll(id);
    REQUIRE(loaded.size() == 4);
    // Ascending, contiguous, and structurally identical to what went in --
    // this is what resume seeds the loop's working history with.
    for (std::size_t i = 0; i < loaded.size(); ++i) {
        CHECK(loaded[i].seq == static_cast<std::int64_t>(i));
    }
    CHECK(MessagesEqual(loaded[0].message, UserMsg("first")));
    CHECK(MessagesEqual(loaded[1].message, tool_use));
    CHECK(MessagesEqual(loaded[2].message, llm::Message{llm::Role::User, {result}}));
    CHECK(MessagesEqual(loaded[3].message,
                         llm::Message{llm::Role::Assistant, {llm::TextBlock{"last"}}}));
    CHECK(loaded[3].token_count == 17);

    // Paging and full-load agree on content; they are two views of one table.
    const auto page = reader.GetMessages(id, std::nullopt, 100);
    REQUIRE(page.messages.size() == loaded.size());
    for (std::size_t i = 0; i < loaded.size(); ++i) {
        CHECK(page.messages[loaded.size() - 1 - i].seq == loaded[i].seq);
    }
}

TEST_CASE("LoadMessages on an empty or unknown session is empty, not an error",
          "[session][resume]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "empty.db");
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    CHECK(SessionReader(db).LoadAll(id).empty());
    CHECK(SessionReader(db).LoadAll("no-such-session").empty());
}

TEST_CASE("compaction checkpoints round-trip, upsert, and clear", "[session][checkpoint]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "ckpt.db");
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    db.AppendMessage(id, UserMsg("one"));
    db.AppendMessage(id, UserMsg("two"));

    // No checkpoint until one is written -- and that is clean, not an error.
    CHECK_FALSE(db.LoadCompactionCheckpoint(id).has_value());
    CHECK_FALSE(db.LoadCompactionCheckpoint("no-such-session").has_value());

    const llm::Message summary{llm::Role::User,
                               {llm::TextBlock{"earlier context was compacted"}}};
    db.SaveCompactionCheckpoint(id, 0, summary);

    const auto loaded = db.LoadCompactionCheckpoint(id);
    REQUIRE(loaded.has_value());
    CHECK(loaded->session_id == id);
    CHECK(loaded->boundary_seq == 0);
    CHECK(loaded->created_at_unix_ms > 0);
    REQUIRE(MessagesEqual(loaded->summary, summary));

    // Upsert: the second compaction replaces, never stacks.
    const llm::Message newer{llm::Role::User, {llm::TextBlock{"covers more"}}};
    db.SaveCompactionCheckpoint(id, 1, newer);
    const auto replaced = db.LoadCompactionCheckpoint(id);
    REQUIRE(replaced.has_value());
    CHECK(replaced->boundary_seq == 1);
    CHECK(MessagesEqual(replaced->summary, newer));

    // The resume tail is everything the summary does not cover.
    db.AppendMessage(id, UserMsg("three"));
    const auto tail = SessionReader(db).LoadAfter(id, replaced->boundary_seq);
    REQUIRE(tail.size() == 1);
    CHECK(tail[0].seq == 2);
    CHECK(MessagesEqual(tail[0].message, UserMsg("three")));

    db.ClearCompactionCheckpoint(id);
    CHECK_FALSE(db.LoadCompactionCheckpoint(id).has_value());
}

TEST_CASE("deleting a session cascades to its compaction checkpoint", "[session][checkpoint]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "ckpt-cascade.db");
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    db.SaveCompactionCheckpoint(id, 3, llm::Message{llm::Role::User, {llm::TextBlock{"s"}}});

    REQUIRE(db.DeleteSession(id));
    CHECK_FALSE(db.LoadCompactionCheckpoint(id).has_value());
}

TEST_CASE("the checkpoint runs on the reaper's threshold, not the append's",
          "[session][checkpoint]") {
    // wal_autocheckpoint=0 moved checkpoint I/O off the committing append (it
    // used to run under mu_, on the tool-result path). The reaper's tick pays
    // it instead, gated by an append threshold: below it, a tick is a no-op;
    // at it, one RESTART checkpoint moves the accumulated frames into the main
    // database AND resets the log (PASSIVE would leave the WAL growing without
    // bound once autocheckpoint is disabled). The pragma invariant above
    // proves the write path never self-checkpoints; this proves the reaper's
    // counter does.
    TempDir dir;
    const auto path = dir.path() / "ckpt.db";
    SessionDatabase db(path);
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;

    for (int i = 0; i < SessionDatabase::kCheckpointAppends - 1; ++i) {
        db.AppendMessage(id, UserMsg(std::string(64, 'x')), 0);
    }
    CHECK_FALSE(db.MaybeRunPassiveCheckpoint());  // below the threshold: no-op

    db.AppendMessage(id, UserMsg("threshold"), 0);
    CHECK(db.MaybeRunPassiveCheckpoint());  // at the threshold: runs

    // The log reset RESTART sets up takes effect on the NEXT writer
    // transaction: the WAL restarts from its head, so a fresh reader finds
    // only that append's frames, not the ~kCheckpointAppends the threshold
    // produced. (Before the next write, the WAL file still physically holds
    // the backfilled frames -- resetting is about where the writer goes, not
    // about erasing bytes.)
    db.AppendMessage(id, UserMsg("after the checkpoint"), 0);
    sqlite3* raw = RawOpen(path);
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw, "PRAGMA wal_checkpoint(PASSIVE);", -1, &stmt,
                               nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    const int frames_left = sqlite3_column_int(stmt, 1);
    sqlite3_finalize(stmt);
    sqlite3_close(raw);
    CHECK(frames_left < 20);  // one append's footprint; the backlog is ~1000 frames
}

TEST_CASE("append latency: the checkpoint on the write path vs the reaper's tick",
          "[.bench]") {
    // Documents the wal_autocheckpoint=0 change by execution, the "verify by
    // execution" bar for a performance claim. The OLD shape is SQLite's
    // default (checkpoint every ~1000 WAL pages): a committing append crosses
    // the threshold and synchronously absorbs the checkpoint -- visible as a
    // spike in per-append time. The NEW shape pays it on the reaper's tick
    // instead. Run explicitly: minimal_tests "[.bench]".
    using clock = std::chrono::steady_clock;
    const std::string body(256 * 1024, 'x');  // a real tool result's size class
    constexpr int kAppends = 2000;

    auto run = [&](int autocheckpoint) {
        TempDir dir;
        const auto path = dir.path() / "bench.db";
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        char* err = nullptr;
        const std::string pragma =
            "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=" +
            std::to_string(autocheckpoint) + ";";
        REQUIRE(sqlite3_exec(db, pragma.c_str(), nullptr, nullptr, &err) == SQLITE_OK);
        REQUIRE(sqlite3_exec(db, "CREATE TABLE m (id INTEGER PRIMARY KEY, body TEXT);", nullptr,
                             nullptr, &err) == SQLITE_OK);

        long long total_us = 0;
        long long max_us = 0;
        for (int i = 0; i < kAppends; ++i) {
            sqlite3_stmt* stmt = nullptr;
            const auto start = clock::now();
            REQUIRE(sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err) == SQLITE_OK);
            REQUIRE(sqlite3_prepare_v2(db, "INSERT INTO m (body) VALUES (?);", -1, &stmt,
                                       nullptr) == SQLITE_OK);
            sqlite3_bind_text(stmt, 1, body.data(), static_cast<int>(body.size()),
                              SQLITE_TRANSIENT);
            REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
            REQUIRE(sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err) == SQLITE_OK);
            const auto us =
                std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start)
                    .count();
            total_us += us;
            max_us = std::max(max_us, us);
        }
        sqlite3_close(db);
        return std::pair<long long, long long>{total_us, max_us};
    };

    const auto on_append = run(1000);  // the old shape: SQLite's default
    const auto on_reaper = run(0);     // the new shape: reaper-gated
    WARN("append latency " << kAppends << " x 256KiB -- checkpoint-on-append: total "
                           << on_append.first << "us, worst append " << on_append.second
                           << "us | checkpoint-on-reaper: total " << on_reaper.first
                           << "us, worst append " << on_reaper.second << "us");
}

TEST_CASE("LastMessageSeq reports the highest seq, and -1 when there are none",
          "[session][resume]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "lastseq.db");
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;

    // -1, not 0: seq 0 is a real first message, so the empty state must not
    // collide with it.
    CHECK(db.LastMessageSeq(id) == -1);
    CHECK(db.LastMessageSeq("no-such-session") == -1);

    db.AppendMessage(id, UserMsg("one"));
    db.AppendMessage(id, UserMsg("two"));
    db.AppendMessage(id, UserMsg("three"));
    CHECK(db.LastMessageSeq(id) == 2);
}

TEST_CASE("a store opened on a stored transcript continues its sequence",
          "[session][resume]") {
    // The resume invariant: reopening a session's store must not restart seq.
    // Getting this wrong would restart last_appended_seq at -1 while SQL's
    // MAX(seq)+1 keeps assigning the real continuation -- and TurnSummary's
    // bounds would describe the previous incarnation of the session.
    TempDir dir;
    SessionDatabase db(dir.path() / "continue.db");
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    db.AppendMessage(id, UserMsg("stored one"));
    db.AppendMessage(id, UserMsg("stored two"));
    db.AppendMessage(id, UserMsg("stored three"));

    SqliteHistoryStore store(db, id);
    CHECK(store.last_appended_seq() == 2);  // before ANY append of its own

    store.Append(UserMsg("after resume"), std::nullopt);
    CHECK(store.last_appended_seq() == 3);

    const auto loaded = SessionReader(db).LoadAll(id);
    REQUIRE(loaded.size() == 4);
    CHECK(loaded[3].seq == 3);
    CHECK(MessagesEqual(loaded[3].message, UserMsg("after resume")));
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
//
// Phase 5: the free-default-naming RULE (which text counts, how it is
// shaped) moved to agent::DefaultSessionName (session_naming_test.cpp) --
// AppendMessage no longer inspects message content at all. What remains a
// SessionDatabase concern is the guard: SetDefaultName only takes effect
// while the session still has no name, and records the source.

TEST_CASE("SetDefaultName names an unnamed session, once", "[session][naming]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "naming.db");
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;

    CHECK(db.SetDefaultName(id, "Fix the login bug on the auth service"));
    CHECK(db.GetSession(id)->name == "Fix the login bug on the auth service");

    // A later call never clobbers the name -- and reports it did not.
    CHECK_FALSE(db.SetDefaultName(id, "now also handle the logout path please"));
    CHECK(db.GetSession(id)->name == "Fix the login bug on the auth service");
}

TEST_CASE("an explicit creation name blocks the default", "[session][naming]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "explicit.db");
    const auto id = db.CreateSession("my chosen name", std::nullopt, "/p", "build").session_id;
    CHECK_FALSE(db.SetDefaultName(id, "hello there"));
    CHECK(db.GetSession(id)->name == "my chosen name");
}

TEST_CASE("UpdateSessionName renames and reports unknown ids", "[session][naming]") {
    TempDir dir;
    SessionDatabase db(dir.path() / "rename.db");
    const auto id = db.CreateSession("old", std::nullopt, "/p", "build").session_id;
    CHECK(db.UpdateSessionName(id, "new"));
    CHECK(db.GetSession(id)->name == "new");
    CHECK_FALSE(db.UpdateSessionName("no-such-session", "x"));
}
TEST_CASE("name_source records where a name came from", "[session][naming]") {
    // NOTE: the name_source column is asserted through the SessionDatabase's
    // own GetSession, never through a second in-process connection. The test
    // binary links its own SQLite copy for the raw fixtures (see CMakeLists)
    // while server_core embeds another -- and two SQLite libraries in one
    // process do not share their connection registry, so a reader's close
    // would treat itself as the last connection and delete a live writer's
    // WAL. Same-process second connections against a live SessionDatabase are
    // therefore off-limits in these tests.
    TempDir dir;
    SessionDatabase db(dir.path() / "source.db");

    // Explicit at creation.
    const auto explicit_id = db.CreateSession("chosen", std::nullopt, "/p", "build").session_id;
    CHECK(db.IsExplicitlyNamed(explicit_id));
    CHECK(db.GetSession(explicit_id)->name_source == "explicit");

    // Default rule -- not explicit, and the column says so.
    const auto default_id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    CHECK(db.SetDefaultName(default_id, "fix the login bug"));
    CHECK_FALSE(db.IsExplicitlyNamed(default_id));
    CHECK(db.GetSession(default_id)->name_source == "default");

    // A rename marks it as observer-set. The rename does NOT clear the
    // explicit flag on a session the user named -- but the rename seam refuses
    // those before reaching here, so the observable path is default/observer.
    CHECK(db.UpdateSessionName(default_id, "a better title"));
    CHECK(db.GetSession(default_id)->name_source == "observer");
    CHECK_FALSE(db.IsExplicitlyNamed(default_id));
}

TEST_CASE("deleting a large transcript leaves nothing behind", "[session][delete]") {
    // The chunked-delete rewrite changed the stall shape (chunks interleave
    // with other sessions' work), not the result: every message, child,
    // and checkpoint is gone, seq accounting included, and a second delete
    // reports false.
    TempDir dir;
    SessionDatabase db(dir.path() / "big.db");
    const auto parent = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    const auto child = db.CreateSession("", parent, "/p", "build").session_id;
    for (int i = 0; i < 3000; ++i) {  // > 6 chunks of 500
        db.AppendMessage(parent, UserMsg(std::string(32, 'x') + std::to_string(i)));
    }
    db.AppendMessage(child, UserMsg("child row"));
    db.SaveCompactionCheckpoint(parent, 10, llm::Message{llm::Role::User, {llm::TextBlock{"s"}}});

    REQUIRE(db.DeleteSession(parent));
    CHECK(db.ListSessions({.include_subagents = true}).empty());
    CHECK(db.LoadMessages(parent).empty());
    CHECK(db.LoadMessages(child).empty());
    CHECK_FALSE(db.LoadCompactionCheckpoint(parent).has_value());
    CHECK_FALSE(db.DeleteSession(parent));  // already gone, reported honestly
}

TEST_CASE("the startup orphan sweep removes rows whose session is gone",
          "[session][delete][invariant]") {
    // This binary deletes messages before the session row, so its own crash
    // mid-delete cannot create orphans -- the sweep exists for databases
    // written by paths without that promise (foreign_keys off, legacy tools).
    // Written by such a path here: raw sqlite, foreign keys off.
    TempDir dir;
    const auto path = dir.path() / "orphans.db";
    std::string id;
    {
        SessionDatabase db(path);
        id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
        db.AppendMessage(id, UserMsg("real"));
    }
    {
        sqlite3* raw = RawOpen(path);
        char* err = nullptr;
        REQUIRE(sqlite3_exec(raw,
                             "INSERT INTO messages (session_id, seq, role, content_json,"
                             " token_count, created_at) VALUES ('ghost-session', 0, 'user',"
                             " '{\"role\":\"user\",\"content\":[]}', 0, 0);",
                             nullptr, nullptr, &err) == SQLITE_OK);
        REQUIRE(sqlite3_exec(raw,
                             "INSERT INTO compactions (session_id, boundary_seq,"
                             " summary_content_json, created_at) VALUES ('ghost-session', 0,"
                             " '{}', 0);",
                             nullptr, nullptr, &err) == SQLITE_OK);
        sqlite3_close(raw);
    }

    SessionDatabase reopened(path);  // the sweep runs at open

    // The orphans are gone; the real rows are untouched.
    CHECK(RawScalar(path, "SELECT COUNT(*) FROM messages WHERE session_id = 'ghost-session';") ==
          "0");
    CHECK(RawScalar(path,
                    "SELECT COUNT(*) FROM compactions WHERE session_id = 'ghost-session';") ==
          "0");
    REQUIRE(reopened.LoadMessages(id).size() == 1);
    CHECK_FALSE(reopened.LoadCompactionCheckpoint(id).has_value());
}

// --- Crash safety ---------------------------------------------------------------------

TEST_CASE("a second open of the same database is refused, holder pid named",
          "[session][store][invariant]") {
    // The single-writer assumption the store is built on, made structural: two
    // live servers on one transcript would interleave appends with divergent
    // in-memory working sets -- worse than a crash. The refusal names the
    // holder (in-process or cross-process) so the operator can find it.
    TempDir dir;
    const auto path = dir.path() / "s.db";

    SECTION("in-process: the registry refuses before fcntl is even consulted") {
        SessionDatabase db(path);
        bool threw = false;
        std::string message;
        try {
            SessionDatabase second(path);
        } catch (const std::exception& e) {
            threw = true;
            message = e.what();
        }
        CHECK(threw);
        CHECK(message.find(std::to_string(getpid())) != std::string::npos);
        // The loser did not disturb the winner.
        CHECK(db.SessionExists("no-session") == false);
    }

    SECTION("cross-process: the fcntl lock refuses, the lock file names the pid") {
        // The child holds its own connection open and parks; the parent must
        // be refused and see the child's pid, and the child's exit must
        // release the lock (fcntl locks die with the process -- no staleness).
        const pid_t child = fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            try {
                SessionDatabase db(path);
            } catch (...) {
                _exit(43);  // could not take the lock it was supposed to hold
            }
            _exit(42);  // unreachable while it parks... it does not park
        }
        // The child exits immediately; wait for it, then verify the lock is
        // FREE again (the destructor/clean-exit half of the contract).
        int status = 0;
        REQUIRE(waitpid(child, &status, 0) == child);
        REQUIRE(WEXITSTATUS(status) == 42);

        // Cross-process while HELD: spawn a child that parks holding the lock,
        // then refuse the parent's open, then verify release on SIGKILL.
        const pid_t holder = fork();
        REQUIRE(holder >= 0);
        if (holder == 0) {
            try {
                SessionDatabase db(path);
                const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
                (void)id;
                pause();  // hold the lock until killed
            } catch (...) {
                _exit(44);
            }
            _exit(45);
        }
        usleep(300 * 1000);  // let the holder open and create its session
        bool threw = false;
        std::string message;
        try {
            SessionDatabase second(path);
        } catch (const std::exception& e) {
            threw = true;
            message = e.what();
        }
        REQUIRE(threw);
        CHECK(message.find(std::to_string(holder)) != std::string::npos);

        // SIGKILL the holder: fcntl locks die with the process, so the parent
        // can open again with no staleness to clean up.
        REQUIRE(kill(holder, SIGKILL) == 0);
        int holder_status = 0;
        REQUIRE(waitpid(holder, &holder_status, 0) == holder);
        REQUIRE(WIFSIGNALED(holder_status));

        SessionDatabase reopened(path);
        REQUIRE(reopened.ListSessions({}).size() == 1);
    }
}

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

TEST_CASE("a read that fails mid-scan raises instead of returning a short result",
          "[session][robustness]") {
    // `while (sqlite3_step(stmt) == SQLITE_ROW)` exits on SQLITE_DONE and
    // equally on SQLITE_CORRUPT/BUSY/IOERR, with nothing checking which. The
    // resume path is where that hurts: a truncated read becomes the loop's
    // working history while SqliteHistoryStore keeps appending at MAX(seq)+1, so
    // the session runs on a transcript with a hole in it and nothing says so.
    // The JSON decode under these loops is strict about a row it cannot parse,
    // which made the strict half a guard over the lax half's output.
    TempDir dir;
    const auto path = dir.path() / "sessions.db";
    std::string session_id;
    {
        SessionDatabase db(path);
        session_id = db.CreateSession("s", std::nullopt, "/p", "main").session_id;
        // Enough rows to span several b-tree pages, so corrupting one lands
        // inside the scan rather than before it starts.
        for (int i = 0; i < 400; ++i) {
            db.AppendMessage(session_id,
                             llm::Message{llm::Role::User,
                                          {llm::TextBlock{std::string(200, 'x') +
                                                          std::to_string(i)}}},
                             0);
        }
    }

    // Scribble over the tail of the file: the header and first pages stay valid,
    // so the database opens and the scan begins, and the damage is met partway
    // through.
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(file);
        file.seekp(0, std::ios::end);
        const auto size = file.tellp();
        REQUIRE(size > 8192);
        file.seekp(size / 2);
        const std::string garbage(static_cast<std::size_t>(size) / 4, '\xa5');
        file.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }

    SessionDatabase reopened(path);
    // The point is NOT which error: it is that a failed read is an error at all,
    // rather than a plausible-looking short transcript.
    bool threw = false;
    std::vector<StoredMessage> loaded;
    try {
        loaded = reopened.LoadMessages(session_id);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK((threw || loaded.size() == 400));
    CHECK_FALSE((!threw && loaded.size() < 400));  // never short AND silent
}

TEST_CASE("GetSession and LastMessageSeq refuse to lie about a corrupted database",
          "[session][robustness]") {
    // The un-swept siblings of the SessionExists and mid-scan fixes above:
    // GetSession returned nullopt for SQLITE_BUSY/IOERR/CORRUPT exactly as for
    // an absent row -- a transient fault told the resume handler "no such
    // session" about a session that is on disk -- and LastMessageSeq returned
    // -1 for the same failures, which RunRedo read as "empty transcript" and
    // fed to LoadAfter(-1): the entire transcript loaded as new, duplicated
    // history. nullopt and -1 are the EMPTY answers; a FAILED read must never
    // produce them.
    TempDir dir;
    const auto path = dir.path() / "sessions.db";
    std::string session_id;
    {
        SessionDatabase db(path);
        session_id = db.CreateSession("s", std::nullopt, "/p", "main").session_id;
        for (int i = 0; i < 400; ++i) {
            db.AppendMessage(session_id,
                             llm::Message{llm::Role::User,
                                          {llm::TextBlock{std::string(200, 'x') +
                                                          std::to_string(i)}}},
                             0);
        }
    }

    // Corrupt every page after the first. The first page carries the file
    // header and the schema root -- it must survive, or the reopen itself
    // would refuse before either query runs. Zeroing the start of each
    // following page destroys its b-tree page type, which SQLite detects
    // deterministically: page 2 is the sessions table's root, and the
    // (session_id, discarded, seq) index the MAX(seq) seek walks is among the
    // rest -- the mid-scan test above only reaches the content pages, and a
    // corruption that spares the sessions table fails neither query.
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(file);
        file.seekp(0, std::ios::end);
        const auto size = static_cast<std::streamoff>(file.tellp());
        REQUIRE(size > 2 * 4096);
        for (std::streamoff page = 1; page * 4096 < size; ++page) {
            file.seekp(page * 4096);
            file.write("\0\0\0\0\0\0\0\0", 8);
        }
    }

    SessionDatabase reopened(path);

    bool session_threw = false;
    std::optional<SessionSummary> summary;
    try {
        summary = reopened.GetSession(session_id);
    } catch (const std::exception&) {
        session_threw = true;
    }
    // This session EXISTS. Either the read succeeds and reports it, or the
    // read fails loudly -- never "missing".
    CHECK((session_threw || summary.has_value()));

    bool seq_threw = false;
    std::int64_t last_seq = 0;
    try {
        last_seq = reopened.LastMessageSeq(session_id);
    } catch (const std::exception&) {
        seq_threw = true;
    }
    // -1 means "empty transcript"; this one holds 400 rows. A failed read
    // throws rather than producing the empty answer.
    CHECK((seq_threw || last_seq == 399));
}
