#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/read_file.hpp"

namespace minimal::config {

// Whether a missing file is an error or the file's "nothing configured" state.
enum class AbsencePolicy { kThrow, kDefaultOnMissing };

// Whether a file that exists but is zero bytes is treated the same as a
// missing file, or as malformed JSON (which it is, strictly).
enum class EmptyFilePolicy { kTreatAsMalformed, kTreatAsAbsent };

// The one entry point every JSON config file goes through: check existence
// per `absence`, read the file (common::ReadFile's cap), treat an
// empty-but-present file per `empty_policy`, parse, and hand the document to
// `parse`. Every failure is a ConfigError whose message starts with
// `<label>` (the file name, e.g. "settings.json") -- callers report it
// verbatim. A file that exists but cannot be opened is reported as such, not
// as "not found"; an oversized or unreadable file always throws, whatever
// `absence` says. `T` must be default-constructible for the absent paths.
template <typename T>
T LoadJsonConfig(const std::filesystem::path& path, const std::string& label,
                 AbsencePolicy absence, EmptyFilePolicy empty_policy,
                 const std::function<T(const nlohmann::json&)>& parse) {
    using Status = common::ReadFileResult::Status;
    common::ReadFileResult file = common::ReadFile(path);
    if (file.status == Status::kNotFound) {
        if (absence == AbsencePolicy::kThrow) throw ConfigError(label + ": " + file.error);
        return T{};
    }
    if (!file.ok()) throw ConfigError(label + ": " + file.error);
    if (file.contents.empty() && empty_policy == EmptyFilePolicy::kTreatAsAbsent) return T{};
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(file.contents);
    } catch (const nlohmann::json::exception& error) {
        throw ConfigError(label + ": invalid JSON: " + std::string(error.what()));
    }
    try {
        return parse(doc);
    } catch (const ConfigError&) {
        throw;
    } catch (const nlohmann::json::exception& error) {
        // A stray unguarded get<T>() in a parser: still name the file.
        throw ConfigError(label + ": " + std::string(error.what()));
    }
}

}  // namespace minimal::config
