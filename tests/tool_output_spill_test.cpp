#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "minimal/tools/policy.hpp"
#include "minimal/tools/tool_output_spill.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;

namespace {

std::string Slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("HeadTail keeps the start and the end within the cap", "[tool_output_spill]") {
    std::string text;
    for (int i = 0; i < 1000; ++i) text += "line-" + std::to_string(i) + "\n";

    CHECK(tools::HeadTail("short", 100) == "short");

    const auto cut = tools::HeadTail(text, 400);
    CHECK(cut.rfind("line-0\n", 0) == 0);
    CHECK(cut.find("line-999\n") != std::string::npos);
    CHECK(cut.find("bytes omitted") != std::string::npos);
    CHECK(cut.size() < 400 + 48);
}

TEST_CASE("HeadTail never splits a UTF-8 character at either cut", "[tool_output_spill]") {
    const std::string euro = "\xE2\x82\xAC";  // 3 bytes
    std::string text;
    for (int i = 0; i < 200; ++i) text += euro;  // 600 bytes, no ASCII to land on

    const auto cut = tools::HeadTail(text, 100);
    const auto marker = cut.find("\n[...");
    REQUIRE(marker != std::string::npos);
    CHECK(marker % 3 == 0);  // the head is whole characters
    const auto tail_start = cut.find("...]\n") + 5;
    CHECK((cut.size() - tail_start) % 3 == 0);  // so is the tail
}

TEST_CASE("CapWithSpill saves the whole text and names it; within the cap nothing is written",
          "[tool_output_spill]") {
    TempDir dir;
    const std::string big(10000, 'x');

    tools::ToolOutputSpill none(dir.path(), "s1", "small.txt");
    CHECK(tools::CapWithSpill("tiny", 100, none) == "tiny");
    CHECK_FALSE(std::filesystem::exists(dir.path() / ".minimal"));

    tools::ToolOutputSpill spill(dir.path(), "s1", "call-1.txt");
    const auto text = tools::CapWithSpill(big, 1000, spill);
    CHECK(text.find("[Output cut to 1000 of 10000 bytes") != std::string::npos);
    CHECK(text.find(".minimal/tool-output/s1/call-1.txt") != std::string::npos);
    CHECK(Slurp(dir.path() / ".minimal/tool-output/s1/call-1.txt") == big);
    // Scoped to tool-output/ alone: other .minimal/ content stays tracked.
    CHECK(Slurp(dir.path() / ".minimal/tool-output/.gitignore") == "*\n");
    CHECK_FALSE(std::filesystem::exists(dir.path() / ".minimal/.gitignore"));
}

TEST_CASE("a spill that cannot be written says so instead of naming a missing file",
          "[tool_output_spill]") {
    tools::ToolOutputSpill spill(std::filesystem::path(), "s1", "x.txt");
    const auto text = tools::CapWithSpill(std::string(5000, 'y'), 1000, spill);
    CHECK_FALSE(spill.ok());
    CHECK(text.find("Saving the full output failed") != std::string::npos);
    CHECK(text.find(".minimal/tool-output") == std::string::npos);
}

TEST_CASE("a backend-issued name cannot walk out of the session dir", "[tool_output_spill]") {
    TempDir dir;
    tools::ToolOutputSpill spill(dir.path(), "../s1", "../../escape.txt");
    spill.Append("data");
    spill.Finish();
    REQUIRE(spill.ok());
    CHECK(spill.RelativePath().find("..") == std::string::npos);
    CHECK_FALSE(std::filesystem::exists(dir.path().parent_path() / "escape.txt"));
}

TEST_CASE("a session dir keeps only the newest spills", "[tool_output_spill]") {
    TempDir dir;
    const auto session_dir = tools::ToolOutputDir(dir.path(), "s1");
    for (std::size_t i = 0; i < tools::kSpillKeepPerSession + 5; ++i) {
        tools::ToolOutputSpill spill(dir.path(), "s1", "f" + std::to_string(i) + ".txt");
        spill.Append("x");
        spill.Finish();
        // Distinct, increasing mtimes in the past regardless of filesystem
        // resolution, so each fresh write is the newest when it prunes.
        std::filesystem::last_write_time(
            session_dir / ("f" + std::to_string(i) + ".txt"),
            std::filesystem::file_time_type::clock::now() - std::chrono::seconds(1000 - i));
    }
    // One more write prunes against the mtimes set above.
    tools::ToolOutputSpill last(dir.path(), "s1", "last.txt");
    last.Append("x");
    last.Finish();

    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(session_dir)) ++count;
    CHECK(count == tools::kSpillKeepPerSession);
    CHECK_FALSE(std::filesystem::exists(session_dir / "f0.txt"));  // oldest went first
    CHECK(std::filesystem::exists(session_dir / ("f" + std::to_string(tools::kSpillKeepPerSession + 4) + ".txt")));
}
