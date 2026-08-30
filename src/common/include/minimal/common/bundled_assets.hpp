#pragma once

// Where the files that ship WITH the binary live, as opposed to the ones the
// user owns (ConfigDir) or the ones the server writes (DataDir).
//
// Two layouts have to work, which is the whole reason these are functions
// rather than constants: an installed tree (<prefix>/bin/minimal-server, assets
// under <prefix>/lib and <prefix>/share) and a build tree (build/src/server/
// minimal-server, assets staged under build/). Both are resolved relative to the
// running executable, so a relocated install and a `cmake --build` tree both
// find their own assets and never each other's.

#include <filesystem>
#include <string>
#include <vector>

namespace minimal::common {

// Empty when the platform has no way to answer (neither macOS nor Linux).
std::filesystem::path ExecutablePath();

// Bundled plugin .so/.dylib directory. May not exist; discovery treats a missing
// directory as "no bundled plugins".
std::filesystem::path BundledPluginDir();

// Bundled prompt directory -- the source of the default prompts/compression.md
// that gets seeded into the config dir on first run. May not exist.
std::filesystem::path BundledPromptDir();

// Copies every file in `bundled_dir` that `config_dir/prompts/` does not already
// have, and returns the names written. Called at startup, before anything reads
// the config dir: AgentPresetRegistry loads prompts/compression.md at
// construction, and an absent one silently disables compaction for every
// session -- the compactor degrades to "no compaction" and says so only in a log.
//
// Same reasoning as writing plugins.json on first run: a prompt the user cannot
// see is a prompt they cannot change, and this project's premise is that they
// can change everything.
//
// NEVER OVERWRITES. An existing file is the user's, edited or not; silently
// reverting someone's compaction prompt on upgrade would be a far worse failure
// than shipping without one.
//
// Best-effort -- a read-only config dir or a missing bundle returns empty rather
// than throwing. Making an absent prompt loud belongs to the startup sanity check.
//
// `bundled_dir` is a parameter rather than a call to BundledPromptDir() so this
// is testable without depending on where the calling binary happens to sit.
std::vector<std::string> SeedBundledPrompts(const std::filesystem::path& bundled_dir,
                                            const std::filesystem::path& config_dir);

}  // namespace minimal::common
