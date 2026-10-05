#pragma once

// Where the files that ship WITH the binary live.
//
// There is exactly one such kind left: bundled plugin libraries. Everything else
// under bundled/ -- prompts, presets, settings -- is copied into the user's
// config dir by `cmake --install` (see cmake/SeedConfig.cmake) and read from
// there, so it has no runtime lookup path at all. That difference is deliberate
// and is the whole rule: a plugin is code the user does not edit, a prompt is
// text they should, and text they cannot see is text they cannot change.
//
// Plugins therefore need a two-layout probe that config files do not: an installed tree (<prefix>/bin/minimal-server, libraries under
// <prefix>/lib) and a build tree (build/src/server/minimal-server, libraries
// under build/plugins). Both resolve relative to the running executable, so a
// relocated install and a `cmake --build` tree find their own and never each
// other's.

#include <filesystem>

namespace minimal::common {

// Empty when the platform has no way to answer (neither macOS nor Linux).
std::filesystem::path ExecutablePath();

// Bundled plugin .so/.dylib directory. May not exist; discovery treats a missing
// directory as "no bundled plugins".
std::filesystem::path BundledPluginDir();

// The client twin: .../plugins/client in both layouts (MINIMAL_CLIENT_PLUGIN_DIR
// is the same value CMake installs to). Same probe, same missing-directory rule.
std::filesystem::path BundledClientPluginDir();

}  // namespace minimal::common
