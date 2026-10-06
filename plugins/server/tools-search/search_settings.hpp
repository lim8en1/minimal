#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>
#include "minimal/common/units.hpp"
#include "policy.hpp"

namespace minimal::tools_search {

// This plugin's own config, the "config" object on its plugins.json entry,
// with one sub-object per tool: {"grep": {...}, "glob": {...}}. Core keeps it
// as raw JSON precisely so each plugin owns its own schema -- this is that
// schema, and nothing outside tools-search needs to know it. Same shape and
// rules as tools-fs's FsSettings: every field optional, missing values keep
// the struct's defaults.
struct SearchSettings {
    // grep -- head_limit the model sees when it does not supply one, the
    // absolute ceiling on collected results, and the hard wall-clock bound on
    // one invocation. The timeout exists for the same reason bash's does: a
    // session worker blocks the whole session, so traversal must be able to
    // give up and report partial results. (RE2 already guarantees the regex
    // itself cannot wedge the worker; the timeout bounds the WALK.)
    int grep_default_head_limit = kDefaultGrepHeadLimit;
    int grep_max_results = kDefaultGrepMaxResults;
    std::int64_t grep_max_file_bytes = kDefaultGrepMaxFileBytes;
    int grep_timeout_ms = static_cast<int>(kDefaultWalkTimeout.count());
    int grep_max_context_lines = kDefaultGrepMaxContextLines;
    int grep_max_line_length = kDefaultGrepMaxLineLength;

    // glob -- a full tree listing bounded the same way. glob_max_results caps
    // the rows one call RETURNS (head_limit's ceiling); the walk is bounded by
    // the timeout alone, so the reported total stays exact.
    int glob_default_limit = kDefaultGlobLimit;
    int glob_max_results = kDefaultGlobMaxResults;
    int glob_timeout_ms = static_cast<int>(kDefaultWalkTimeout.count());
};

SearchSettings ParseSearchSettings(const nlohmann::json& config);

}  // namespace minimal::tools_search
