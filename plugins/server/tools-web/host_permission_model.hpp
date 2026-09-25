#pragma once

// tools-web's permission model (tools/permission_model.hpp): per-HOST rules
// for its own two tools, replacing core's `network` scalar fold for them.
// Everything else stays core -- the base mode (read_only category, per-tool
// rules), out_of_workdir, ceilings, run modes, grants, the human.
//
// Slices, keyed by this plugin's name ("tools-web"):
//
//   permissions.json   "plugins": {"tools-web": {"default": "ask",
//                                                "hosts": {"*.internal": "deny",
//                                                          "docs.python.org": "allow"}}}
//   preset             plugin_permissions:
//                        tools-web: {hosts: {"*.github.com": allow}}
//
// Host globs are matched case-insensitively against the whole host ('*'
// matches any run, including dots). The most SPECIFIC matching glob wins
// (most literal characters; a tie goes to the stricter mode), so a broad
// "*.example.com": ask can carve out "api.example.com": allow. A preset's rule
// replaces the operator's rule for the same glob; distinct globs coexist.
// An unmatched host takes the layer's `default` (the preset's, else the
// operator's), else core's `network` mode -- installing this plugin changes
// nothing until a slice says something.
//
// A project-local (untrusted) preset may only NARROW: its answer for a host
// is folded most-restrictively with what the operator's slice alone answers.
//
// Which host: web_fetch -- the `url` argument's host; web_search -- the
// configured search backend's host (the only host a search contacts). A
// call whose host cannot be parsed takes the default. Redirects never cross
// hosts on a decision made for another: web_fetch refuses a cross-host hop
// and hands the model the target URL, whose fetch is gated on its own.

#include <memory>
#include <string>

#include "minimal/tools/permission_model.hpp"
#include "web_settings.hpp"

namespace minimal::tools_web {

inline constexpr const char* kPluginName = "tools-web";

std::shared_ptr<const tools::PermissionModel> MakeHostPermissionModel(WebSettings settings);

}  // namespace minimal::tools_web
