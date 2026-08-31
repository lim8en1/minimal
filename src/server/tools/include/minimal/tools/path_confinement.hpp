#pragma once

#include <filesystem>
#include <string>

namespace minimal::tools {

struct ConfineResult {
    std::filesystem::path absolute_path;
    bool ok = false;                  // false only if the path cannot be safely
                                       // resolved at all (see confine() below)
    bool outside_project_root = false;
    std::string error;                // set iff !ok
};

// Canonicalizes `user_supplied` (resolved relative to project_root) and reports
// whether the result escapes project_root, rather than hard-rejecting it -- the
// caller (permission gating) decides what to do with an outside-root path.
//
// Still fails closed (ok=false) when the path cannot be safely resolved at all:
// an embedded NUL, a symlink loop, or any other condition the filesystem itself
// cannot canonicalize. Encoded-traversal strings (e.g. "%2e%2e") are not special
// here -- std::filesystem never URL-decodes, so they resolve as inert filenames,
// which is what defeats that attack class rather than a blacklist check.
//
// must_exist selects std::filesystem::canonical (requires the full path to
// exist) vs weakly_canonical (resolves the existing prefix, lexically normalizes
// the rest) -- Read/Edit's `view` need the former, Write's create-new-file path
// needs the latter.
//
// THE CLASSIFICATION IS NOT A LOCK. confine() canonicalizes a PATH and returns
// it; the file at that path can be swapped -- a symlink renamed onto it, a
// directory component replaced -- between this classification and the tool's
// later open(), so a deliberately racing process can route an operation outside
// the root. This is a known and accepted asymmetry, the same one the bash
// section of ARCHITECTURE.md owns: the threat is model ERROR, not a hostile
// local process, and against a hostile local process only a container is a
// boundary. Hardening here (O_NOFOLLOW reopen, fstat-vs-stat comparison) would
// add real complexity to every consumer and defend against a threat the
// permission model explicitly does not claim to stop. Revisit only if the
// server is ever exposed beyond the local single-user topology.
ConfineResult confine(const std::filesystem::path& project_root,
                       const std::string& user_supplied,
                       bool must_exist);

}  // namespace minimal::tools
