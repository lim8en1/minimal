#pragma once

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace minimal::testing {

// A unique temp directory removed on scope exit. Filesystem tools have to be
// tested against a real filesystem -- permissions, rename semantics, and
// directory-vs-file distinctions are exactly what a fake would paper over.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("minimal-test-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_);
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
