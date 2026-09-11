#pragma once

#include <cstdint>
#include <sqlite3.h>

#include <string>

namespace minimal::common {

std::int64_t NowMillis();

[[noreturn]] void ThrowSqliteError(sqlite3* db, const std::string& context);

void ExecChecked(sqlite3* db, const char* sql, const std::string& context);

// RAII wrapper around a one-shot prepared statement. Callers that reuse
// statements across calls (a hot path) want their own cache instead.
struct Stmt {
    sqlite3_stmt* stmt = nullptr;
    explicit Stmt(sqlite3* db, const char* sql, const std::string& context) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            ThrowSqliteError(db, context);
        }
    }
    ~Stmt() {
        if (stmt) sqlite3_finalize(stmt);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
};

// Best-effort ROLLBACK, ignoring any error from the rollback itself -- for use
// in a `catch (...) { BestEffortRollback(db); throw; }` migration/transaction
// guard, where the original exception must propagate regardless of whether
// the rollback succeeds.
void BestEffortRollback(sqlite3* db);

}  // namespace minimal::common
