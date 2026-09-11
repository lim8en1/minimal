#pragma once

// Shared read/write primitives for the client's small JSON preference files
// (ui.json, client-plugins.json): a read failure must never be mistaken for
// "the file is empty," and a write must never truncate the real file in
// place -- both files used to get this wrong in slightly different,
// independently-incomplete ways before this header existed.

#include <filesystem>
#include <string>
#include <string_view>

namespace minimal::client {

// Reads `path` into `*out`. The caller must already know the file EXISTS
// (check std::filesystem::exists separately) before calling this -- a read
// failure here (permission denied, a transient I/O error) is distinct from
// "doesn't exist" and must never be treated as "the file is empty," or a
// save right after would silently overwrite the user's real (merely
// unreadable) file with fresh/default content.
bool TryReadConfigFile(const std::filesystem::path& path, std::string* out);

// Atomic temp+rename write: a sibling ".tmp" file in the SAME directory (so
// the rename stays within one filesystem and is therefore atomic), written
// in full, then renamed over `path`. A crash or a second process's write
// mid-write leaves at most a harmless .tmp file behind, never a truncated or
// torn target. Returns false on any failure (the temp write, or the
// rename); the caller logs its own domain-specific warning.
bool WriteConfigFileAtomic(const std::filesystem::path& path, std::string_view contents);

}  // namespace minimal::client
