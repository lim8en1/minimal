// sqlite_util's small set of shared helpers.
//
// NowMillis and Stmt's success path are tested directly here. ExecChecked,
// ThrowSqliteError, BestEffortRollback, and Stmt's OWN failure path are NOT:
// all four are (or call into) functions defined out-of-line in
// sqlite_util.cpp, compiled once into minimal_common, and that static archive
// is absorbed into the SHARED minimal_server_core -- so calling them from
// THIS executable resolves to the dylib's copy of sqlite3's internal state,
// while a `sqlite3*` opened by a raw sqlite3_open() call made directly in
// this file (or in any test binary) resolves against SQLite3::SQLite3 linked
// straight into the test executable -- a SEPARATE, independently-initialized
// copy of the same static library. Mixing the two crashes inside sqlite3_exec
// or sqlite3_errmsg (confirmed by `nm`: sqlite3_open/sqlite3_exec are DEFINED
// in the test binary itself, while ExecChecked/ThrowSqliteError/
// BestEffortRollback are UNDEFINED there, i.e. resolved dynamically to the
// dylib -- a handle from one copy fed to the other's internal call tables is
// not a supported combination; confirmed by reproducing the crash both via
// ExecChecked directly and via Stmt's failure path, which calls
// ThrowSqliteError internally). Stmt's SUCCESS path never calls
// ThrowSqliteError, and its other methods are defined INLINE in the header,
// so a Stmt used here compiles against THIS file's own (executable-side)
// sqlite3 symbols -- consistent with a handle this same file opened -- which
// is what keeps the test below safe. Everything else is exercised indirectly
// instead, through SessionDatabase's and PluginStorage's existing test
// coverage, which never crosses this boundary because all of their sqlite3
// calls -- open through close -- stay inside the dylib.

#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "minimal/common/sqlite_util.hpp"

using minimal::common::NowMillis;
using minimal::common::Stmt;

TEST_CASE("NowMillis returns a plausible, monotonically-nondecreasing epoch millis",
          "[sqlite_util]") {
    const auto now_wall =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const auto a = NowMillis();
    // Within a generous window of wall-clock time -- catches a unit mistake
    // (seconds vs. millis) without being flaky about exact timing.
    CHECK(a > now_wall - 5000);
    CHECK(a < now_wall + 5000);

    const auto b = NowMillis();
    CHECK(b >= a);
}

namespace {

// Raw sqlite3, opened and torn down with the same directly-linked symbols
// Stmt below will use -- see the file comment for why that consistency
// matters here.
class MemDb {
public:
    MemDb() { sqlite3_open(":memory:", &db_); }
    ~MemDb() { sqlite3_close(db_); }
    sqlite3* get() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

}  // namespace

TEST_CASE("Stmt prepares valid SQL, steps it, and finalizes on scope exit", "[sqlite_util]") {
    MemDb db;
    REQUIRE(sqlite3_exec(db.get(), "CREATE TABLE t (id INTEGER)", nullptr, nullptr, nullptr) ==
            SQLITE_OK);
    REQUIRE(sqlite3_exec(db.get(), "INSERT INTO t VALUES (42)", nullptr, nullptr, nullptr) ==
            SQLITE_OK);

    {
        Stmt stmt(db.get(), "SELECT id FROM t", "select");
        REQUIRE(stmt.stmt != nullptr);
        REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_ROW);
        CHECK(sqlite3_column_int(stmt.stmt, 0) == 42);
    }  // finalized here; a leaked statement would keep an implicit transaction open

    // If the destructor did not finalize, this DROP would fail with "table t
    // is locked" (SQLITE_LOCKED) under a lingering prepared statement.
    CHECK(sqlite3_exec(db.get(), "DROP TABLE t", nullptr, nullptr, nullptr) == SQLITE_OK);
}

// Stmt's failure path (invalid SQL) calls ThrowSqliteError -- see the file
// comment for why that cannot be safely exercised from this binary. It is
// covered instead wherever a migration or query already runs invalid-at-
// runtime SQL against a real SessionDatabase/PluginStorage in the existing
// suite.
