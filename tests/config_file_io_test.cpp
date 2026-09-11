#include "minimal/client/config_file_io.hpp"

#include <filesystem>
#include <fstream>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

namespace minimal::client {

namespace {

const std::filesystem::path TestDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / ("minimal-config-file-io-" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// Permission-based tests are meaningless run as root: root bypasses POSIX
// read permission checks entirely, so a chmod-000'd file would still open
// fine and the test would spuriously pass regardless of whether the
// production code distinguishes "unreadable" from "empty" correctly.
bool RunningAsRoot() { return ::geteuid() == 0; }

}  // namespace

TEST_CASE("TryReadConfigFile reads a normal file's contents") {
    const auto dir = TestDir("read-ok");
    const auto path = dir / "f.json";
    {
        std::ofstream out(path);
        out << R"({"a": 1})";
    }
    std::string contents;
    CHECK(TryReadConfigFile(path, &contents));
    CHECK(contents == R"({"a": 1})");
    std::filesystem::remove_all(dir);
}

TEST_CASE("TryReadConfigFile fails, not silently empties, on an unreadable file") {
    if (RunningAsRoot()) {
        WARN("skipped: running as root, which bypasses POSIX read permissions");
        return;
    }
    const auto dir = TestDir("read-unreadable");
    const auto path = dir / "f.json";
    {
        std::ofstream out(path);
        out << R"({"a": 1})";
    }
    std::filesystem::permissions(path, std::filesystem::perms::none);
    std::string contents = "unchanged";
    CHECK_FALSE(TryReadConfigFile(path, &contents));
    // The out-param contract: a failed read must not clobber what the
    // caller already had (TryReadConfigFile returns before touching *out).
    CHECK(contents == "unchanged");
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write);
    std::filesystem::remove_all(dir);
}

TEST_CASE("WriteConfigFileAtomic creates a fresh file") {
    const auto dir = TestDir("write-fresh");
    const auto path = dir / "f.json";
    CHECK(WriteConfigFileAtomic(path, R"({"theme": "dark"})"));
    std::string contents;
    REQUIRE(TryReadConfigFile(path, &contents));
    CHECK(contents == R"({"theme": "dark"})");
    CHECK_FALSE(std::filesystem::exists(dir / "f.json.tmp"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("WriteConfigFileAtomic overwrites an existing file and leaves no tmp sibling") {
    const auto dir = TestDir("write-overwrite");
    const auto path = dir / "f.json";
    {
        std::ofstream out(path);
        out << "old contents";
    }
    CHECK(WriteConfigFileAtomic(path, "new contents"));
    std::string contents;
    REQUIRE(TryReadConfigFile(path, &contents));
    CHECK(contents == "new contents");
    CHECK_FALSE(std::filesystem::exists(dir / "f.json.tmp"));
    std::filesystem::remove_all(dir);
}

}  // namespace minimal::client
