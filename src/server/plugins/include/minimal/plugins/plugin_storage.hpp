#pragma once

// Per-plugin persistent storage: one shared SQLite KV store, namespaced by
// plugin.
//
// WHY THIS EXISTS. Plugins increasingly own state that should survive a
// process restart (a checklist per session, caches, small settings), and the
// alternative was every plugin rolling its own file format in the data dir --
// with each author re-deriving SQLite's concurrency discipline, or worse,
// getting it wrong. Core already owns that discipline for the session store
// (one connection, one mutex, WAL, migrations); this is the same mechanism,
// extracted and namespaced. "A plugin must call these, never reimplement" is
// the same rule that put path_confinement on the ABI surface.
//
// THE NAMESPACE IS THE KEY, NOT A GUARANTEE. `plugin` is part of the primary
// key and every method takes it as its first argument, but it is a
// caller-supplied string -- the host hands each plugin its own name and trusts
// it to pass that name back. Isolation therefore holds by convention and
// discipline, not structurally: a buggy (or hostile) plugin that passes
// another plugin's name can read and overwrite its rows. The host does not
// mediate per-call, so a plugin that wants another plugin's data goes through
// the service channel (ServiceRegistry, the sanctioned pull path), not around
// this class.
//
// VALUES ARE OPAQUE STRINGS. Plugins serialize their own JSON (everything else
// in this tree does); core defines neither schema nor semantics. A plugin
// needing a real schema (its own tables, queries) provisions its own database
// in PluginRegistrationContext::data_dir instead -- the documented escape
// hatch -- and inherits the concurrency discipline as its own problem.
//
// LIFETIME: constructed by main before plugin load (the pointer rides
// PluginRegistrationContext into every registration hook) and held for the
// process lifetime, matching the registries and the never-dlclosed plugin
// handles. Every public method locks, because callers span session workers,
// the command pool, and observer dispatch.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;  // opaque handle; <sqlite3.h> stays private to the .cpp

namespace minimal::common {
class ExclusiveFileLock;  // the <db>.lock guard; held open for the process lifetime
}  // namespace minimal::common

namespace minimal::plugins {

class PluginStorage {
public:
    static constexpr int kSchemaVersion = 1;

    // Opens (creating if needed) and migrates. Throws std::runtime_error on
    // any failure, including a database whose user_version is newer than this
    // binary understands -- fatal by design, mirroring SessionDatabase: a
    // plugin store a newer binary wrote must not be silently rebuilt over.
    explicit PluginStorage(const std::filesystem::path& db_path);
    ~PluginStorage();

    PluginStorage(const PluginStorage&) = delete;
    PluginStorage& operator=(const PluginStorage&) = delete;

    // Stored value for (plugin, key); nullopt when absent. Absent is a normal
    // state, not an error.
    std::optional<std::string> Get(std::string_view plugin, std::string_view key) const;

    // Upsert. Throws on I/O failure -- a plugin's Save is allowed to fail
    // loudly rather than silently lose state.
    void Set(std::string_view plugin, std::string_view key, std::string_view value);

    // True when a row was actually removed.
    bool Delete(std::string_view plugin, std::string_view key);

    // Every key of `plugin` beginning with `prefix`, sorted by key. Empty
    // prefix = all keys of the plugin. The PK (plugin, key) makes this a range
    // scan, not a table scan.
    std::vector<std::string> Keys(std::string_view plugin, std::string_view prefix = "") const;

    // Deletes every key of `plugin` beginning with `prefix`; returns the count
    // removed. The cleanup path for plugin-owned per-session state (prefix =
    // "<session_id>/").
    std::size_t DeletePrefix(std::string_view plugin, std::string_view prefix);

    const std::filesystem::path& path() const { return path_; }

private:
    // Declared BEFORE db_ so it is destroyed AFTER the connection closes: the
    // database is never open while the lock file does not name this process.
    std::unique_ptr<common::ExclusiveFileLock> lock_;
    sqlite3* db_ = nullptr;  // opaque here; the .cpp includes <sqlite3.h>
    mutable std::mutex mu_;
    std::filesystem::path path_;
};

}  // namespace minimal::plugins
