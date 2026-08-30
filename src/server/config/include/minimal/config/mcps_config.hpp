#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::config {

enum class McpTransport { Stdio, Http };

// One mcps.json entry. `command`/`args`/`env` are meaningful for stdio,
// `url`/`headers` for http; the other side's fields stay empty.
//
// `env` and `headers` values may contain ${ENV_VAR} interpolation markers. They
// are stored RAW here: resolving them is the MCP client's job at connection
// time (it must not log the resolved values, and a variable set after startup
// should still work). The startup sanity check only needs the raw `command`
// and `url`.
struct McpServerEntry {
    std::string name;
    McpTransport transport = McpTransport::Stdio;
    std::string command;
    std::vector<std::string> args;
    std::unordered_map<std::string, std::string> env;
    std::string url;
    std::unordered_map<std::string, std::string> headers;
    bool enabled = true;  // authoring an entry is an intent to use it; disabling is opt-out
};

struct McpsConfig {
    std::vector<McpServerEntry> entries;
    // Per-entry structural problems (missing name/transport/command/url, unknown
    // transport). A bad entry degrades to a recorded reason rather than taking
    // the good ones down -- the same shape as SkillRegistry's load_error, and
    // what lets the sanity check name each broken piece individually.
    std::vector<std::string> errors;

    const McpServerEntry* Find(const std::string& name) const;
};

// Pure. Throws only on a file-level problem: a top level that is not an array.
// Per-entry problems land in `errors`, never as exceptions.
McpsConfig ParseMcpsConfig(const nlohmann::json& doc);

// Thin: reads the file and parses it. A MISSING file is not an error -- most
// installs configure no MCP servers at all, so empty is the normal state,
// unlike settings/providers/permissions where absence is a broken install.
// Throws if the file exists but isn't valid JSON, or per ParseMcpsConfig above.
McpsConfig LoadMcpsConfig(const std::filesystem::path& path);

}  // namespace minimal::config
