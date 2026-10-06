#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::config {

// A config problem that refuses the file. what() is complete and printable
// as-is ("settings.json: memory.auto_compact: must be true or false, got
// \"yes\""): callers report it verbatim and never prefix it again.
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Where non-fatal problems go (a startup warning, a plugin's report_warning).
// Receives the same fully-qualified text a ConfigError carries.
using WarningSink = std::function<void(const std::string&)>;

enum class UnknownKeys { kError, kWarn };

// A typed, path-tracking cursor over one JSON object of a config file -- the
// one way every config loader reads fields. Rules it enforces everywhere:
//   - a present key of the wrong type is an error naming file and path, never
//     a silent fallback to the default;
//   - integers must be JSON integers (4096.7 is refused, not truncated) and
//     are range-checked before narrowing;
//   - every key read through the cursor is marked consumed, and Finish()
//     reports whatever was not -- the known-key set IS the code that reads,
//     so it cannot drift from it;
//   - keys starting with "//" are comments, in every file.
class Reader {
public:
    // `node` must outlive the reader. A non-object `node` is an error
    // (`path` empty = the file's top level).
    Reader(const nlohmann::json& node, std::string file, std::string path = {},
           WarningSink warn = {});

    const std::string& file() const { return file_; }
    const std::string& path() const { return path_; }
    const nlohmann::json& node() const { return *node_; }

    // "<file>: <path>.<key>" -- the prefix every message starts with.
    std::string Where(std::string_view key = {}) const;
    [[noreturn]] void Fail(std::string_view key, const std::string& message) const;
    void Warn(std::string_view key, const std::string& message) const;

    bool Has(std::string_view key) const;
    // The raw value (consumed), or nullptr when absent. For plugin-owned
    // slices and verbatim passthrough; prefer the typed readers.
    const nlohmann::json* Raw(std::string_view key);
    // Mark a key consumed without reading it (a field handled elsewhere).
    void Accept(std::string_view key);

    std::optional<bool> MaybeBool(std::string_view key);
    bool Bool(std::string_view key, bool fallback) { return MaybeBool(key).value_or(fallback); }

    // `allow_empty` false: "" is an error (a required-looking value left blank).
    std::optional<std::string> MaybeString(std::string_view key, bool allow_empty = true);
    std::string String(std::string_view key, std::string fallback, bool allow_empty = true);
    std::string RequireString(std::string_view key);

    std::optional<std::int64_t> MaybeInt(
        std::string_view key, std::int64_t min = std::numeric_limits<std::int64_t>::min(),
        std::int64_t max = std::numeric_limits<std::int64_t>::max());
    std::int64_t Int(std::string_view key, std::int64_t fallback,
                     std::int64_t min = std::numeric_limits<std::int64_t>::min(),
                     std::int64_t max = std::numeric_limits<std::int64_t>::max());

    std::optional<double> MaybeDouble(std::string_view key,
                                      double min = -std::numeric_limits<double>::infinity(),
                                      double max = std::numeric_limits<double>::infinity());
    double Double(std::string_view key, double fallback,
                  double min = -std::numeric_limits<double>::infinity(),
                  double max = std::numeric_limits<double>::infinity());

    // A string that must be one of `choices`; the error lists them.
    std::optional<std::string> MaybeEnum(std::string_view key,
                                         std::initializer_list<std::string_view> choices);
    std::string Enum(std::string_view key, std::string fallback,
                     std::initializer_list<std::string_view> choices);

    // Absent -> nullopt; present but not an array of strings -> error.
    std::optional<std::vector<std::string>> MaybeStringList(std::string_view key,
                                                            bool allow_empty_entries = false);
    std::vector<std::string> StringList(std::string_view key, bool allow_empty_entries = false);

    // An object of string values. Keys and values are screened for CR/LF:
    // these maps become HTTP headers or child environments.
    std::optional<std::map<std::string, std::string>> MaybeStringMap(std::string_view key);

    // A nested object as its own cursor (absent -> nullopt, non-object ->
    // error). The child's Finish() is the caller's to call.
    std::optional<Reader> MaybeObject(std::string_view key);
    // Same, but absent reads as an empty object.
    Reader Object(std::string_view key);

    // Each non-comment entry of an object-valued key, as (name, value). For
    // maps keyed by user-chosen names (providers, models, rules).
    std::vector<std::pair<std::string, const nlohmann::json*>> Entries(std::string_view key);

    // A child cursor over an arbitrary node reached through Entries() or an
    // array, extending this cursor's path.
    Reader Child(const nlohmann::json& node, const std::string& segment) const;

    // Reports every key not consumed (comments excluded): kError throws,
    // kWarn sends one warning per key.
    void Finish(UnknownKeys policy = UnknownKeys::kError);

private:
    const nlohmann::json* Find(std::string_view key);
    std::string Join(std::string_view key) const;

    const nlohmann::json* node_;
    std::string file_;
    std::string path_;
    WarningSink warn_;
    std::set<std::string, std::less<>> consumed_;  // present and read
    std::set<std::string, std::less<>> queried_;   // every key the code asked for
};

bool IsCommentKey(std::string_view key);

}  // namespace minimal::config
