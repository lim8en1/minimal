#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace minimal::config {

// Whether a missing file is an error or the file's "nothing configured" state.
enum class AbsencePolicy { kThrow, kDefaultOnMissing };

// Whether a file that exists but is zero bytes is treated the same as a
// missing file, or as malformed JSON (which it is, strictly). Only hooks.json
// treats empty-as-absent today; every other loader treats an empty-but-present
// file as a parse failure like any other malformed content.
enum class EmptyFilePolicy { kTreatAsMalformed, kTreatAsAbsent };

// The shape shared by every `Load*Config` function: check existence per
// `absence`, read the file, treat an empty-but-present file per
// `empty_policy`, parse top-level JSON (wrapping a stray
// nlohmann::json::exception into "<label>: invalid JSON: <what>"), then hand
// the parsed doc to `parse`. `T` must be default-constructible for the
// kDefaultOnMissing/kTreatAsAbsent paths.
//
// Deliberately does not read via minimal::common::ReadFileOrEmpty: that
// helper collapses "could not open" and "opened but empty" into the same
// empty-string result, which would erase exactly the distinction
// AbsencePolicy and EmptyFilePolicy exist to preserve.
template <typename T>
T LoadJsonConfig(const std::filesystem::path& path, const std::string& label,
                 AbsencePolicy absence, EmptyFilePolicy empty_policy,
                 const std::function<T(const nlohmann::json&)>& parse) {
    std::ifstream in(path);
    if (!in) {
        if (absence == AbsencePolicy::kThrow) {
            throw std::runtime_error(label + ".json not found: " + path.string());
        }
        return T{};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string contents = buffer.str();
    if (contents.empty() && empty_policy == EmptyFilePolicy::kTreatAsAbsent) {
        return T{};
    }
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(contents);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(label + ".json: invalid JSON: " + std::string(error.what()));
    }
    return parse(doc);
}

}  // namespace minimal::config
