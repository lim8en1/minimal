// ExclusiveFileLock's contract: exclusive across processes (fcntl) AND within
// one process (the in-process registry, since fcntl locks do not conflict
// with themselves across fds of the same process) -- see file_lock.hpp.

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "minimal/store/file_lock.hpp"
#include "temp_dir.hpp"

using minimal::store::ExclusiveFileLock;
using minimal::testing::TempDir;

TEST_CASE("a second lock on the same db path in this process is refused",
          "[file_lock][invariant]") {
    TempDir dir;
    const auto db_path = dir.path() / "sessions.db";

    ExclusiveFileLock first(db_path);
    CHECK_THROWS_AS(ExclusiveFileLock(db_path), std::runtime_error);

    // A DIFFERENT file is unaffected -- the registry key is the file, not a
    // process-wide "one lock ever" rule.
    CHECK_NOTHROW(ExclusiveFileLock(dir.path() / "other.db"));
}

TEST_CASE("the same-process refusal holds for every spelling of the path",
          "[file_lock][invariant]") {
    // Keyed by (device, inode), not by the path string: "./" or a symlinked
    // directory must not let a second in-process holder through, because
    // fcntl would not stop it (its locks never conflict within a process).
    TempDir dir;
    std::filesystem::create_directory(dir.path() / "data");
    std::filesystem::create_directory_symlink(dir.path() / "data", dir.path() / "link");
    ExclusiveFileLock first(dir.path() / "data" / "sessions.db");

    CHECK_THROWS_AS(ExclusiveFileLock(dir.path() / "data" / "." / "sessions.db"),
                    std::runtime_error);
    CHECK_THROWS_AS(ExclusiveFileLock(dir.path() / "link" / "sessions.db"), std::runtime_error);
}

TEST_CASE("releasing the lock (destruction) allows re-acquiring it", "[file_lock][invariant]") {
    TempDir dir;
    const auto db_path = dir.path() / "sessions.db";

    {
        ExclusiveFileLock first(db_path);
    }  // released here: both the fcntl lock (fd closed) and the in-process registry entry

    CHECK_NOTHROW(ExclusiveFileLock(db_path));
}

TEST_CASE("the lock file records this process's pid", "[file_lock]") {
    TempDir dir;
    const auto db_path = dir.path() / "sessions.db";
    ExclusiveFileLock lock(db_path);

    std::ifstream in(db_path.string() + ".lock");
    REQUIRE(in.is_open());
    std::string pid_line;
    std::getline(in, pid_line);
    CHECK(pid_line == std::to_string(::getpid()));
}

TEST_CASE("the same-process refusal names the holder's pid", "[file_lock]") {
    TempDir dir;
    const auto db_path = dir.path() / "sessions.db";
    ExclusiveFileLock held(db_path);

    try {
        ExclusiveFileLock second(db_path);
        FAIL("expected ExclusiveFileLock to throw");
    } catch (const std::runtime_error& e) {
        const std::string message = e.what();
        CHECK(message.find(std::to_string(::getpid())) != std::string::npos);
    }
}

TEST_CASE("a symlink planted at the lock path is refused, not followed",
          "[file_lock][invariant]") {
    // The lock file is opened O_CREAT then ftruncate'd to 0: without
    // O_NOFOLLOW, a symlink planted at <db_path>.lock before construction
    // would make that ftruncate a zero-out of whatever file the symlink
    // points at, not of the lock file itself -- a write primitive into a
    // non-private data dir (the MINIMAL_DATA_DIR escape hatch, or any shared
    // location the db path lives under).
    TempDir dir;
    const auto db_path = dir.path() / "sessions.db";
    const auto lock_path = db_path.string() + ".lock";
    const auto target = dir.path() / "target-not-to-be-truncated.txt";
    {
        std::ofstream out(target);
        out << "do not touch";
    }
    REQUIRE(::symlink(target.c_str(), lock_path.c_str()) == 0);

    CHECK_THROWS_AS(ExclusiveFileLock(db_path), std::runtime_error);

    std::ifstream in(target);
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(contents == "do not touch");
}
