#pragma once

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

namespace minimal::testing {

// A unique temp directory removed on scope exit. Filesystem tools have to be
// tested against a real filesystem -- permissions, rename semantics, and
// directory-vs-file distinctions are exactly what a fake would paper over.
//
// Uniqueness cannot rest on the pid alone: each TEST_CASE is its own process
// (catch_discover_tests), the counter restarts at 0 every time, and a run
// that was killed mid-test (ctest TIMEOUT, a wedge, a crash) leaves the
// directory behind. macOS recycles pids aggressively, so the next process
// with that pid would otherwise build its "fresh" directory inside the stale
// one -- leftover files then fail any exact-listing assertion (observed
// flaking in mention_resolver_test). The name therefore carries a random
// component, and the constructor refuses to build on top of an existing
// path rather than silently sharing it.
//
// THE NAME MUST STAY SHORT: fixtures bind unix domain sockets under this
// path, macOS rejects socket paths past 103 characters, and a spawned
// server's socket appends "/minimal/spawn-test.sock" (24 chars) to it --
// the 8-hex suffix is what remains of the budget after that.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        static std::mt19937_64 rng(
            std::random_device{}() ^
            static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        for (;;) {
            char suffix[9];
            std::snprintf(suffix, sizeof(suffix), "%08x",
                          static_cast<uint32_t>(rng()));
            path_ = std::filesystem::temp_directory_path() /
                    ("mt-" + std::to_string(::getpid()) + "-" +
                     std::to_string(counter.fetch_add(1)) + "-" + suffix);
            std::error_code ec;
            if (!std::filesystem::exists(path_, ec) &&
                std::filesystem::create_directory(path_, ec)) {
                return;
            }
        }
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace minimal::testing
