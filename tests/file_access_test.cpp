#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>

#include "minimal/tools/file_access.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;

namespace {

std::filesystem::path WriteFile(const TempDir& dir, const std::string& name,
                                 const std::string& content) {
    const auto path = dir.path() / name;
    std::ofstream out(path, std::ios::binary);
    out << content;
    return path;
}

}  // namespace

TEST_CASE("reads a whole small file", "[file_access]") {
    TempDir dir;
    const auto path = WriteFile(dir, "a.txt", "one\ntwo\nthree\n");

    const auto window = tools::read_window(path, 1, 100, 2000);

    REQUIRE(window.ok);
    CHECK(window.lines == std::vector<std::string>{"one", "two", "three"});
    CHECK(window.start_line == 1);
    CHECK(window.end_line == 3);
    CHECK(window.total_lines == 3);
    CHECK_FALSE(window.truncated);
}

TEST_CASE("windows by offset and limit, reporting an accurate total", "[file_access]") {
    TempDir dir;
    std::string content;
    for (int i = 1; i <= 100; ++i) content += "line" + std::to_string(i) + "\n";
    const auto path = WriteFile(dir, "big.txt", content);

    const auto window = tools::read_window(path, 10, 5, 2000);

    REQUIRE(window.ok);
    CHECK(window.lines.size() == 5);
    CHECK(window.lines.front() == "line10");
    CHECK(window.lines.back() == "line14");
    CHECK(window.start_line == 10);
    CHECK(window.end_line == 14);
    // The full count is what makes the pagination hint truthful -- it costs a
    // full pass, and reporting a partial total would be worse than useless.
    CHECK(window.total_lines == 100);
    CHECK(window.truncated);
}

TEST_CASE("a window reaching the end is not marked truncated", "[file_access]") {
    TempDir dir;
    const auto path = WriteFile(dir, "a.txt", "one\ntwo\nthree\n");

    const auto window = tools::read_window(path, 2, 2, 2000);

    REQUIRE(window.ok);
    CHECK(window.lines == std::vector<std::string>{"two", "three"});
    CHECK_FALSE(window.truncated);
}

TEST_CASE("an offset past EOF is an empty window, not an error", "[file_access]") {
    // The natural end of paging; treating it as failure would make the model
    // think something went wrong when it simply finished.
    TempDir dir;
    const auto path = WriteFile(dir, "a.txt", "one\ntwo\n");

    const auto window = tools::read_window(path, 99, 10, 2000);

    REQUIRE(window.ok);
    CHECK(window.lines.empty());
    CHECK(window.total_lines == 2);
    CHECK(window.start_line == 0);
}

TEST_CASE("an empty file reads as zero lines", "[file_access]") {
    TempDir dir;
    const auto path = WriteFile(dir, "empty.txt", "");

    const auto window = tools::read_window(path, 1, 10, 2000);

    REQUIRE(window.ok);
    CHECK(window.lines.empty());
    CHECK(window.total_lines == 0);
}

TEST_CASE("a missing trailing newline still yields the last line", "[file_access]") {
    TempDir dir;
    const auto path = WriteFile(dir, "a.txt", "one\ntwo");

    const auto window = tools::read_window(path, 1, 10, 2000);

    REQUIRE(window.ok);
    CHECK(window.lines == std::vector<std::string>{"one", "two"});
    CHECK(window.total_lines == 2);
}

TEST_CASE("CRLF line endings do not leak a stray carriage return", "[file_access]") {
    // Otherwise a control character lands verbatim in the model's context.
    TempDir dir;
    const auto path = WriteFile(dir, "crlf.txt", "one\r\ntwo\r\n");

    const auto window = tools::read_window(path, 1, 10, 2000);

    REQUIRE(window.ok);
    CHECK(window.lines == std::vector<std::string>{"one", "two"});
}

TEST_CASE("over-long lines are truncated visibly", "[file_access]") {
    TempDir dir;
    const auto path = WriteFile(dir, "long.txt", std::string(500, 'x') + "\nshort\n");

    const auto window = tools::read_window(path, 1, 10, 100);

    REQUIRE(window.ok);
    REQUIRE(window.lines.size() == 2);
    CHECK(window.lines[0].size() > 100);  // marker appended
    CHECK(window.lines[0].find("truncated") != std::string::npos);
    CHECK(window.lines[1] == "short");
}

TEST_CASE("binary files are refused rather than dumped as mojibake", "[file_access]") {
    TempDir dir;
    const auto path = WriteFile(dir, "bin.dat", std::string("PNG\x00\x01\x02binary", 14));

    const auto window = tools::read_window(path, 1, 10, 2000);

    CHECK_FALSE(window.ok);
    CHECK(window.is_binary);
    CHECK_FALSE(window.error.empty());
}

TEST_CASE("a directory is refused", "[file_access]") {
    TempDir dir;
    const auto window = tools::read_window(dir.path(), 1, 10, 2000);
    CHECK_FALSE(window.ok);
}

TEST_CASE("a missing file is an error, not a crash", "[file_access]") {
    TempDir dir;
    const auto window = tools::read_window(dir.path() / "nope.txt", 1, 10, 2000);
    CHECK_FALSE(window.ok);
    CHECK_FALSE(window.error.empty());
}

TEST_CASE("a large file is windowed without retaining it", "[file_access]") {
    // Memory proportional to `limit`, not file size -- the property that makes
    // reading a very large file safe at all.
    TempDir dir;
    std::string content;
    for (int i = 0; i < 200000; ++i) content += "some line of text here\n";
    const auto path = WriteFile(dir, "huge.txt", content);

    const auto window = tools::read_window(path, 100000, 10, 2000);

    REQUIRE(window.ok);
    CHECK(window.lines.size() == 10);
    CHECK(window.total_lines == 200000);
    CHECK(window.truncated);
}
