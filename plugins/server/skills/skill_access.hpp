#pragma once

// Per-preset skill access: what the operator's permissions.json `plugins.skills`
// slice and a preset's `plugin_permissions.skills` slice say about each skill.
//
//   {"*": "allow", "style": "preload", "internal": "ask", "secret": "deny"}
//
// A list (["a", "b"]) is shorthand for every entry = allow. Absent = deny.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "skill_registry.hpp"

namespace minimal::skills {

// The ORDERING is the subagent clamp lattice: Preload is the WIDEST mode (the
// body ships in the system prompt, always on) and Deny the narrowest, so
// clamping a child takes the elementwise MAX -- a child can never widen into
// its parent's preloads, and a parent's deny beats every child request.
enum class SkillMode {
    Preload = 0,  // body appended to the system prompt; not callable via the tool
    Allow = 1,    // listed; the `skill` tool loads it outright
    Ask = 2,      // listed; loading prompts (grant key `skill:<name>`)
    Deny = 3,     // absent from the listing; the `skill` tool is refused
};

using SkillAccess = std::map<std::string, SkillMode>;

// Parses one slice (null -> empty). Throws std::runtime_error naming `where`.
SkillAccess ParseSkillAccess(const nlohmann::json& slice, const std::string& where);

// The mode one name gets: its own entry, else "*", else Deny.
SkillMode ModeFor(const SkillAccess& access, const std::string& name);

// `access` against the root's loaded skills: "*" expanded, unknown and
// not-loaded names dropped, Deny entries dropped (absence IS deny). Sorted,
// so every rendering of it is deterministic (it sits in the cached prefix).
SkillAccess ResolveForRoot(const SkillAccess& access, const std::vector<SkillMeta>& merged);

// Elementwise most-restrictive against the parent's resolved map; a skill the
// parent lacks is deny.
SkillAccess ClampToParent(const SkillAccess& child, const SkillAccess& parent);

// "Available skills:\n- name: description" for the Allow/Ask entries (no
// trailing newline -- it is appended to the tool description), "" if none.
std::string RenderListing(const SkillAccess& resolved, const std::vector<SkillMeta>& merged);

// The preloaded bodies under one heading, "" if none. A preload whose body
// cannot be read is skipped with a warning (it is neither in the prompt nor
// callable -- the gate denies preload entries).
std::string RenderPreloads(const SkillAccess& resolved, const SkillRegistry& registry,
                           const std::filesystem::path& project_root);

}  // namespace minimal::skills
