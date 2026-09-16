// ReadFileOrEmpty's failure contract: loud on cap and I/O error, silent only
// on "cannot open".

#include <catch2/catch_test_macros.hpp>

#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "minimal/common/read_file.hpp"
#include "temp_dir.hpp"

using minimal::common::ReadFileOrEmpty;

TEST_CASE("ReadFileOrEmpty reads a file and tolerates absence") {
    minimal::testing::TempDir dir;
    const auto path = dir.path() / "f.txt";
    {
        std::ofstream out(path);
        out << "hello";
    }
    CHECK(ReadFileOrEmpty(path) == "hello");
    CHECK(ReadFileOrEmpty(dir.path() / "missing.txt") == "");
    // An empty file: the rdbuf insertion sets failbit, which is NOT an error.
    { std::ofstream out(dir.path() / "empty.txt"); }
    CHECK(ReadFileOrEmpty(dir.path() / "empty.txt") == "");
}

TEST_CASE("ReadFileOrEmpty throws past the size cap instead of truncating") {
    minimal::testing::TempDir dir;
    const auto path = dir.path() / "big.txt";
    {
        std::ofstream out(path, std::ios::binary);
        out << std::string(128, 'x');
    }
    CHECK_THROWS_AS(ReadFileOrEmpty(path, 64), std::runtime_error);
    // Under the cap: fine.
    CHECK(ReadFileOrEmpty(path, 128) == std::string(128, 'x'));
}

TEST_CASE("ReadFileOrEmpty rejects an over-cap file without reading it whole") {
    // Needs a file past the cap that the bounded read then refuses -- 4 GiB,
    // sparse. The resize itself is a write, so a sandbox whose RLIMIT_FSIZE
    // sits below it (some CI containers pin it at 1 GiB) kills the test with
    // SIGXFSZ before the case can test anything. A low soft cap can be raised
    // unprivileged as long as the hard cap allows, so try raising it to
    // exactly what this case needs; if the OS refuses, the environment
    // genuinely cannot host the resize and the case says so instead of dying.
    const rlim_t kSparseResize = 4ull << 30;
    rlimit fsize{};
    REQUIRE(::getrlimit(RLIMIT_FSIZE, &fsize) == 0);
    if (fsize.rlim_cur < kSparseResize) {
        fsize.rlim_cur = kSparseResize;
        if (::setrlimit(RLIMIT_FSIZE, &fsize) != 0) {
            SKIP("RLIMIT_FSIZE below the 4 GiB sparse resize -- "
                 "environment cap, not the code under test");
        }
    }

    // The cap used to be checked AFTER `buffer << in.rdbuf()` had already
    // slurped the entire file, so it bounded nothing: a huge (or hostile)
    // file was fully allocated before the throw. A sparse multi-gigabyte file
    // costs no real disk space to create, and the fix reads in bounded chunks
    // and stops one byte past the cap -- so this must throw near-instantly,
    // not after however long it takes to read gigabytes.
    minimal::testing::TempDir dir;
    const auto path = dir.path() / "huge.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out << "x";
    }
    std::error_code ec;
    std::filesystem::resize_file(path, 4ull << 30, ec);  // 4 GiB, sparse
    REQUIRE_FALSE(ec);

    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(ReadFileOrEmpty(path, 1024), std::runtime_error);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // Generous bound: bounded-chunk reading rejects this in well under a
    // second; slurping 4 GiB first would take far longer (or fail with
    // bad_alloc) on any machine this suite runs on.
    CHECK(elapsed < std::chrono::seconds(5));
}
