#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include "minimal/common/atomic_write.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;

namespace {

std::string ReadBack(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::size_t CountEntries(const std::filesystem::path& dir) {
    std::size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        (void)entry;
        ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("creates a new file and reports it as created", "[atomic_write]") {
    TempDir dir;
    const auto path = dir.path() / "new.txt";

    const auto outcome = common::write_file(path, "hello");

    REQUIRE(outcome.ok);
    CHECK(outcome.created);
    CHECK(outcome.bytes_written == 5);
    CHECK(ReadBack(path) == "hello");
}

TEST_CASE("overwrites an existing file and reports it as not created", "[atomic_write]") {
    TempDir dir;
    const auto path = dir.path() / "existing.txt";
    { std::ofstream(path) << "old content that is longer"; }

    const auto outcome = common::write_file(path, "new");

    REQUIRE(outcome.ok);
    CHECK_FALSE(outcome.created);
    CHECK(ReadBack(path) == "new");  // fully replaced, not partially overwritten
}

TEST_CASE("leaves no temp files behind on success", "[atomic_write]") {
    // The staging file lives in the target's directory (so rename stays within
    // one filesystem and is therefore atomic), which means a leak would be
    // visible right where the user works.
    TempDir dir;
    const auto path = dir.path() / "a.txt";

    REQUIRE(common::write_file(path, "content").ok);

    CHECK(CountEntries(dir.path()) == 1);
}

TEST_CASE("a missing parent directory is refused, not created", "[atomic_write]") {
    // Implicit mkdir -p would turn a typo'd model-supplied path into a new tree.
    TempDir dir;
    const auto path = dir.path() / "nope" / "deep" / "a.txt";

    const auto outcome = common::write_file(path, "content");

    CHECK_FALSE(outcome.ok);
    CHECK(outcome.error.find("parent directory") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(dir.path() / "nope"));
}

TEST_CASE("a failed write leaves the original untouched and no debris",
          "[atomic_write]") {
    TempDir dir;
    const auto path = dir.path() / "subdir";
    std::filesystem::create_directory(path);

    const auto outcome = common::write_file(path, "content");

    CHECK_FALSE(outcome.ok);
    CHECK(std::filesystem::is_directory(path));  // unchanged
    CHECK(CountEntries(dir.path()) == 1);         // no stray temp file
}

TEST_CASE("writes empty content", "[atomic_write]") {
    TempDir dir;
    const auto path = dir.path() / "empty.txt";

    const auto outcome = common::write_file(path, "");

    REQUIRE(outcome.ok);
    CHECK(outcome.bytes_written == 0);
    CHECK(std::filesystem::exists(path));
}

TEST_CASE("writes content containing NUL bytes verbatim", "[atomic_write]") {
    // read_window refuses to *read* binary, but writing is a separate decision:
    // a tool that was told to write these bytes should write exactly them.
    TempDir dir;
    const auto path = dir.path() / "bin.dat";
    const std::string content("a\0b", 3);

    const auto outcome = common::write_file(path, content);

    REQUIRE(outcome.ok);
    CHECK(outcome.bytes_written == 3);
    CHECK(ReadBack(path) == content);
}

TEST_CASE("editing an existing file preserves its mode", "[atomic_write][invariant]") {
    // The rename installs a NEW inode, so the target's permissions survive only
    // if the temp file carries them. The previous unconditional 0644 silently
    // stripped executable bits and widened 0600 private files on their next
    // edit -- a file the user could no longer run, now readable by the group.
    TempDir dir;
    const auto path = dir.path() / "script.sh";
    { std::ofstream(path) << "#!/bin/sh\necho hi\n"; }
    REQUIRE(::chmod(path.c_str(), 0700) == 0);

    REQUIRE(common::write_file(path, "#!/bin/sh\necho bye\n").ok);

    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 07777) == 0700);
}

TEST_CASE("a new file lands with ordinary 0644 permissions, not mkstemp's 0600",
          "[atomic_write]") {
    TempDir dir;
    const auto path = dir.path() / "new.txt";

    REQUIRE(common::write_file(path, "content").ok);

    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 07777) == 0644);
}

TEST_CASE("a target filename near NAME_MAX still writes", "[atomic_write]") {
    TempDir dir;
    const auto path = dir.path() / (std::string(250, 'n') + ".txt");  // 254 bytes: valid

    const auto outcome = common::write_file(path, "content");
    CHECK(outcome.ok);
    CHECK(std::filesystem::exists(path));
}

TEST_CASE("create_perms overrides the default mode for a new file", "[atomic_write]") {
    TempDir dir;
    const auto path = dir.path() / "secret.txt";

    REQUIRE(common::write_file(path, "content",
                              std::filesystem::perms::owner_read |
                                  std::filesystem::perms::owner_write)
                .ok);

    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    CAPTURE(std::oct, st.st_mode & 07777);
    CHECK((st.st_mode & 07777) == 0600);
}
