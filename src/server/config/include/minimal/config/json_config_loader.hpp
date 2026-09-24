#pragma once

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "minimal/common/read_file.hpp"

namespace minimal::config {

// Whether a missing file is an error or the file's "nothing configured" state.
enum class AbsencePolicy { kThrow, kDefaultOnMissing };

// Whether a file that exists but is zero bytes is treated the same as a
// missing file, or as malformed JSON (which it is, strictly). Only hooks.json
// treats empty-as-absent today; every other loader treats an empty-but-present
// file as a parse failure like any other malformed content.
enum class EmptyFilePolicy { kTreatAsMalformed, kTreatAsAbsent };

// The shape shared by every `Load*Config` function: check existence per
// `absence`, read the file (common::ReadFile's cap), treat an empty-but-present
// file per `empty_policy`, parse top-level JSON (wrapping a stray
// nlohmann::json::exception into "<label>: invalid JSON: <what>"), then hand
// the parsed doc to `parse`. `T` must be default-constructible for the
// kDefaultOnMissing/kTreatAsAbsent paths. A file that exists but cannot be
// opened is reported as such, not as "not found"; an oversized or unreadable
// file always throws, whatever `absence` says.
template <typename T>
T LoadJsonConfig(const std::filesystem::path& path, const std::string& label,
                 AbsencePolicy absence, EmptyFilePolicy empty_policy,
                 const std::function<T(const nlohmann::json&)>& parse) {
    using Status = common::ReadFileResult::Status;
    common::ReadFileResult file = common::ReadFile(path);
    if (file.status == Status::kNotFound || file.status == Status::kCannotOpen) {
        if (absence == AbsencePolicy::kThrow) {
            throw std::runtime_error(label + ".json: " + file.error);
        }
        return T{};
    }
    if (!file.ok()) throw std::runtime_error(label + ".json: " + file.error);
    if (file.contents.empty() && empty_policy == EmptyFilePolicy::kTreatAsAbsent) {
        return T{};
    }
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(file.contents);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(label + ".json: invalid JSON: " + std::string(error.what()));
    }
    return parse(doc);
}

}  // namespace minimal::config
