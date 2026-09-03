#include "minimal/common/sqlite_util.hpp"

#include <chrono>
#include <stdexcept>

namespace minimal::common {

std::int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void ThrowSqliteError(sqlite3* db, const std::string& context) {
    throw std::runtime_error(context + ": " + sqlite3_errmsg(db));
}

void ExecChecked(sqlite3* db, const char* sql, const std::string& context) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string message = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error(context + ": " + message);
    }
}

void BestEffortRollback(sqlite3* db) {
    char* err = nullptr;
    if (sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, &err) != SQLITE_OK && err) {
        sqlite3_free(err);
    }
}

}  // namespace minimal::common
