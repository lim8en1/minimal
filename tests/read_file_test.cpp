// ReadFileOrEmpty's failure contract: loud on cap and I/O error, silent only
// on "cannot open".

#include <catch2/catch_test_macros.hpp>

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
