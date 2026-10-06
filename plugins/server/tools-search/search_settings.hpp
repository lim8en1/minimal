#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

#include "minimal/common/live_value.hpp"

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
    int grep_default_head_limit = 250;
    int grep_max_results = 1000;
    std::int64_t grep_max_file_bytes = 1024 * 1024;
    int grep_timeout_ms = 10000;
    int grep_max_context_lines = 10;
    int grep_max_line_length = 2000;

    // glob -- a full tree listing bounded the same way. glob_max_results caps
    // the rows one call RETURNS (head_limit's ceiling); the walk is bounded by
    // the timeout alone, so the reported total stays exact.
    int glob_default_limit = 200;
    int glob_max_results = 2000;
    int glob_timeout_ms = 10000;
};

SearchSettings ParseSearchSettings(const nlohmann::json& config);

// What grep and glob read per call; a reconfigure swaps it.
using LiveSearchSettings = std::shared_ptr<const common::LiveValue<SearchSettings>>;

}  // namespace minimal::tools_search
