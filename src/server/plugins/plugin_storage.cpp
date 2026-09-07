#include "minimal/plugins/plugin_storage.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "minimal/common/file_lock.hpp"
#include "minimal/common/sqlite_util.hpp"

namespace minimal::plugins {
namespace {

using common::ExecChecked;
using common::NowMillis;
using common::Stmt;
using common::ThrowSqliteError;

void BindText(sqlite3_stmt* stmt, int index, std::string_view value, const std::string& context) {
    if (sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        ThrowSqliteError(sqlite3_db_handle(stmt), context);
    }
}

std::string ColumnText(sqlite3_stmt* stmt, int index) {
    const auto* text = sqlite3_column_text(stmt, index);
    const int size = sqlite3_column_bytes(stmt, index);
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(size));
}

void RequireNonEmpty(std::string_view plugin, std::string_view key) {
    if (plugin.empty()) {
        throw std::invalid_argument("plugin storage namespace (plugin name) must not be empty");
    }
    if (key.empty()) {
        throw std::invalid_argument("plugin storage key must not be empty");
    }
}

// Exclusive upper bound for a prefix range scan over BINARY-ordered TEXT.
// Bumping the last non-0xFF byte gives the smallest string greater than every
// string with that prefix; an all-0xFF prefix has no bound (nothing sorts after
// it that still shares it), so the range stays open-ended there.
std::optional<std::string> UpperBound(std::string_view prefix) {
    std::string bound(prefix);
    for (std::size_t i = bound.size(); i-- > 0;) {
        if (static_cast<unsigned char>(bound[i]) < 0xFF) {
            ++bound[i];
            bound.resize(i + 1);
            return bound;
        }
    }
    return std::nullopt;
}

// Reads user_version, refuses a database ahead of this binary, and applies
// pending migrations inside one transaction -- the SessionDatabase discipline,
// so a failure leaves the file exactly as it was opened.
void ApplyMigrations(sqlite3* db, int target_version) {
    ExecChecked(db, "BEGIN IMMEDIATE;", "plugin storage: failed to begin migration");
    try {
        int current = 0;
        {
            Stmt stmt(db, "PRAGMA user_version;", "plugin storage: failed to read schema version");
            if (sqlite3_step(stmt.stmt) == SQLITE_ROW) current = sqlite3_column_int(stmt.stmt, 0);
        }

        if (current > target_version) {
            throw std::runtime_error(
                "plugin storage schema version " + std::to_string(current) +
                " is newer than this binary understands (" + std::to_string(target_version) +
                "); refusing to open -- do not downgrade over a newer database");
        }

        if (current < 1) {
            ExecChecked(db,
                        "CREATE TABLE plugin_kv ("
                        "  plugin TEXT NOT NULL,"
                        "  key TEXT NOT NULL,"
                        "  value TEXT NOT NULL,"
                        "  updated_at INTEGER NOT NULL,"
                        "  PRIMARY KEY (plugin, key)"
                        ");",
                        "plugin storage: migration v1");
        }

        ExecChecked(db, ("PRAGMA user_version = " + std::to_string(target_version) + ";").c_str(),
                    "plugin storage: failed to record schema version");
        ExecChecked(db, "COMMIT;", "plugin storage: failed to commit migration");
    } catch (...) {
        common::BestEffortRollback(db);
        throw;
    }
}

}  // namespace

PluginStorage::PluginStorage(const std::filesystem::path& db_path) : path_(db_path) {
    // The directory FIRST: ExclusiveFileLock opens `db_path + ".lock"`
    // immediately below, and open() with O_CREAT still fails ENOENT when a
    // DIRECTORY component is missing (O_CREAT only covers the leaf file) --
    // a fresh XDG_DATA_HOME/first run on this machine threw "cannot open
    // lock file .../plugins.db.lock: No such file or directory" instead of
    // just working, the same first-run case the session store already
    // handles correctly (session_database.cpp creates its own directory
    // before opening too).
    std::error_code ec;
    if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path(), ec);
    }

    // Same single-writer guarantee as the session store, the same way: a
    // second process on plugins.db is refused at open (see ExclusiveFileLock).
    lock_ = std::make_unique<common::ExclusiveFileLock>(db_path);

    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("failed to open plugin storage " + path_.string() + ": " +
                                 message);
    }

    // Same pragmas as the session store, for the same reasons: WAL for
    // read-while-write and crash-safe last-commit, NORMAL for WAL's recommended
    // durability point, busy_timeout for out-of-process readers. No
    // foreign_keys: the single table has none to enforce.
    for (const char* pragma :
         {"PRAGMA journal_mode=WAL;", "PRAGMA synchronous=NORMAL;", "PRAGMA busy_timeout=5000;"}) {
        ExecChecked(db_, pragma, std::string("plugin storage: failed to set ") + pragma);
    }

    ApplyMigrations(db_, kSchemaVersion);
}

PluginStorage::~PluginStorage() {
    if (db_) {
        // A clean close of the last connection checkpoints the WAL and removes
        // the -wal/-shm side files.
        sqlite3_close(db_);
    }
}

std::optional<std::string> PluginStorage::Get(std::string_view plugin, std::string_view key) const {
    RequireNonEmpty(plugin, key);
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "SELECT value FROM plugin_kv WHERE plugin = ? AND key = ?;",
              "plugin storage: failed to prepare get");
    BindText(stmt.stmt, 1, plugin, "plugin storage: failed to bind get");
    BindText(stmt.stmt, 2, key, "plugin storage: failed to bind get");
    const int rc = sqlite3_step(stmt.stmt);
    if (rc == SQLITE_ROW) return ColumnText(stmt.stmt, 0);
    if (rc == SQLITE_DONE) return std::nullopt;
    ThrowSqliteError(db_, "plugin storage: get failed");
}

void PluginStorage::Set(std::string_view plugin, std::string_view key, std::string_view value) {
    RequireNonEmpty(plugin, key);
    // A per-value quota: the KV seam is the plugin's persistence, not an
    // unbounded scratch space -- one buggy or hostile plugin could otherwise
    // grow the host's plugin database without limit. 1 MiB per value is far
    // above any legitimate state blob (checklist/cost rows are kilobytes).
    constexpr std::size_t kMaxValueBytes = 1u << 20;
    if (value.size() > kMaxValueBytes) {
        throw std::runtime_error("plugin storage: value exceeds the 1 MiB quota (plugin '" +
                                 std::string(plugin) + "', key '" + std::string(key) + "')");
    }
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_,
              "INSERT INTO plugin_kv (plugin, key, value, updated_at) VALUES (?, ?, ?, ?) "
              "ON CONFLICT (plugin, key) DO UPDATE SET value = excluded.value, "
              "updated_at = excluded.updated_at;",
              "plugin storage: failed to prepare set");
    BindText(stmt.stmt, 1, plugin, "plugin storage: failed to bind set");
    BindText(stmt.stmt, 2, key, "plugin storage: failed to bind set");
    BindText(stmt.stmt, 3, value, "plugin storage: failed to bind set");
    if (sqlite3_bind_int64(stmt.stmt, 4, NowMillis()) != SQLITE_OK) {
        ThrowSqliteError(db_, "plugin storage: failed to bind set");
    }
    if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "plugin storage: set failed");
    }
}

bool PluginStorage::Delete(std::string_view plugin, std::string_view key) {
    RequireNonEmpty(plugin, key);
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "DELETE FROM plugin_kv WHERE plugin = ? AND key = ?;",
              "plugin storage: failed to prepare delete");
    BindText(stmt.stmt, 1, plugin, "plugin storage: failed to bind delete");
    BindText(stmt.stmt, 2, key, "plugin storage: failed to bind delete");
    if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "plugin storage: delete failed");
    }
    return sqlite3_changes(db_) > 0;
}

std::vector<std::string> PluginStorage::Keys(std::string_view plugin, std::string_view prefix) const {
    RequireNonEmpty(plugin, "keys-scan");  // prefix may be empty; the namespace may not
    std::lock_guard<std::mutex> lock(mu_);
    const auto upper = UpperBound(prefix);
    // The PK (plugin, key) index answers the (plugin, key range) predicate as a
    // seek; ORDER BY key is satisfied by the same index, so no sort step.
    std::string sql =
        "SELECT key FROM plugin_kv WHERE plugin = ? AND key >= ?";
    if (upper) sql += " AND key < ?";
    sql += " ORDER BY key ASC;";

    Stmt stmt(db_, sql.c_str(), "plugin storage: failed to prepare keys");
    BindText(stmt.stmt, 1, plugin, "plugin storage: failed to bind keys");
    BindText(stmt.stmt, 2, prefix, "plugin storage: failed to bind keys");
    if (upper) BindText(stmt.stmt, 3, *upper, "plugin storage: failed to bind keys");

    std::vector<std::string> keys;
    int rc = 0;
    while ((rc = sqlite3_step(stmt.stmt)) == SQLITE_ROW) {
        keys.push_back(ColumnText(stmt.stmt, 0));
    }
    if (rc != SQLITE_DONE) ThrowSqliteError(db_, "plugin storage: keys scan failed");
    return keys;
}

std::size_t PluginStorage::DeletePrefix(std::string_view plugin, std::string_view prefix) {
    RequireNonEmpty(plugin, "delete-prefix");
    std::lock_guard<std::mutex> lock(mu_);
    const auto upper = UpperBound(prefix);
    std::string sql = "DELETE FROM plugin_kv WHERE plugin = ? AND key >= ?";
    if (upper) sql += " AND key < ?";
    sql += ";";

    Stmt stmt(db_, sql.c_str(), "plugin storage: failed to prepare delete-prefix");
    BindText(stmt.stmt, 1, plugin, "plugin storage: failed to bind delete-prefix");
    BindText(stmt.stmt, 2, prefix, "plugin storage: failed to bind delete-prefix");
    if (upper) BindText(stmt.stmt, 3, *upper, "plugin storage: failed to bind delete-prefix");
    if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "plugin storage: delete-prefix failed");
    }
    return static_cast<std::size_t>(sqlite3_changes(db_));
}

}  // namespace minimal::plugins
