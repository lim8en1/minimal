#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "minimal/config/project_context.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace minimal::config;

namespace {

struct TempProject {
    testing::TempDir tempdir;
    std::filesystem::path dir = tempdir.path();

    void Write(const std::filesystem::path& relative, const std::string& contents) const {
        const auto path = dir / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << contents;
    }
};

ProjectContextSettings Settings(bool search_ancestors = false) {
    ProjectContextSettings settings;
    settings.search_ancestors = search_ancestors;
    return settings;
}

}  // namespace

TEST_CASE("project context: AGENTS.md is found and loaded verbatim", "[project_context]") {
    TempProject project;
    project.Write("AGENTS.md", "Always run the tests.\n");

    const auto context = LoadProjectContext(project.dir, Settings());
    REQUIRE(context.has_value());
    CHECK(context->body == "Always run the tests.\n");
    CHECK(context->file_name == "AGENTS.md");
    std::error_code ec;
    CHECK(context->source_path ==
          std::filesystem::weakly_canonical(project.dir / "AGENTS.md", ec));
    CHECK_FALSE(context->truncated);
}

TEST_CASE("project context: CLAUDE.md is the fallback", "[project_context]") {
    TempProject project;
    project.Write("CLAUDE.md", "Claude-flavored instructions.\n");

    const auto context = LoadProjectContext(project.dir, Settings());
    REQUIRE(context.has_value());
    CHECK(context->file_name == "CLAUDE.md");
    CHECK(context->body == "Claude-flavored instructions.\n");
}

TEST_CASE("project context: file order decides within one directory", "[project_context]") {
    TempProject project;
    project.Write("AGENTS.md", "from agents\n");
    project.Write("CLAUDE.md", "from claude\n");

    const auto context = LoadProjectContext(project.dir, Settings());
    REQUIRE(context.has_value());
    CHECK(context->file_name == "AGENTS.md");
    CHECK(context->body == "from agents\n");
}

TEST_CASE("project context: a directory without either file yields nothing", "[project_context]") {
    TempProject project;
    project.Write("README.md", "no context here\n");

    std::string error;
    CHECK_FALSE(LoadProjectContext(project.dir, Settings(), &error).has_value());
    // Absence is the normal state, not an error.
    CHECK(error.empty());
}

TEST_CASE("project context: disabled yields nothing even with the file present",
          "[project_context]") {
    TempProject project;
    project.Write("AGENTS.md", "ignored\n");

    ProjectContextSettings settings = Settings();
    settings.enabled = false;
    CHECK_FALSE(LoadProjectContext(project.dir, settings).has_value());
}

TEST_CASE("project context: an empty file carries no instructions", "[project_context]") {
    TempProject project;
    project.Write("AGENTS.md", "");
    project.Write("CLAUDE.md", "real instructions\n");

    // Empty AGENTS.md is skipped, and the search CONTINUES to the next
    // candidate rather than stopping at a file that has nothing to say.
    const auto context = LoadProjectContext(project.dir, Settings());
    REQUIRE(context.has_value());
    CHECK(context->file_name == "CLAUDE.md");

    SECTION("whitespace-only counts as empty too") {
        TempProject project2;
        project2.Write("AGENTS.md", " \t\r\n");
        CHECK_FALSE(LoadProjectContext(project2.dir, Settings()).has_value());
    }
}

TEST_CASE("project context: max_bytes truncates with the facts recorded", "[project_context]") {
    TempProject project;
    const std::string body(100, 'x');
    project.Write("AGENTS.md", body);

    ProjectContextSettings settings = Settings();
    settings.max_bytes = 40;

    const auto context = LoadProjectContext(project.dir, settings);
    REQUIRE(context.has_value());
    CHECK(context->body.size() == 40);
    CHECK(context->truncated);
    CHECK(context->total_bytes == 100);
    CHECK(context->body == std::string(40, 'x'));

    SECTION("a file exactly at the cap is not truncated") {
        ProjectContextSettings exact = Settings();
        exact.max_bytes = 100;
        const auto not_cut = LoadProjectContext(project.dir, exact);
        REQUIRE(not_cut.has_value());
        CHECK_FALSE(not_cut->truncated);
        CHECK(not_cut->body == body);
    }
}

TEST_CASE("project context: search_ancestors defaults OFF", "[project_context]") {
    TempProject project;
    project.Write("parent/AGENTS.md", "from the parent\n");
    project.Write("project/.keep", "");

    const auto context = LoadProjectContext(project.dir / "project", Settings());
    CHECK_FALSE(context.has_value());
}

TEST_CASE("project context: search_ancestors walks upward", "[project_context]") {
    TempProject project;
    project.Write("parent/AGENTS.md", "from the parent\n");
    project.Write("parent/project/.keep", "");

    const auto context = LoadProjectContext(project.dir / "parent" / "project",
                                            Settings(/*search_ancestors=*/true));
    REQUIRE(context.has_value());
    CHECK(context->body == "from the parent\n");
    std::error_code ec;
    CHECK(context->source_path ==
          std::filesystem::weakly_canonical(project.dir / "parent" / "AGENTS.md", ec));
}

TEST_CASE("project context: the ancestor walk terminates at the filesystem root",
          "[project_context]") {
    TempProject project;
    project.Write("project/.keep", "");

    // A candidate name that exists nowhere on the walk up to "/" keeps this
    // hermetic: the machine's own /AGENTS.md or /CLAUDE.md must not decide
    // the outcome. The walk must stop at the root, not spin there --
    // parent_path() is a fixed point at "/" and never yields an empty path.
    ProjectContextSettings settings = Settings(/*search_ancestors=*/true);
    settings.files = {"no-such-context-file.md"};

    std::string error;
    CHECK_FALSE(LoadProjectContext(project.dir / "project", settings, &error).has_value());
    CHECK(error.empty());
}

TEST_CASE("project context: proximity beats file order across directories",
          "[project_context][invariant]") {
    TempProject project;
    // AGENTS.md is the PREFERRED name, but it sits farther away. The CLAUDE.md
    // in the project itself is the more specific instruction set and wins --
    // "nearest directory first, then the configured order within it".
    project.Write("parent/AGENTS.md", "from the parent\n");
    project.Write("parent/project/CLAUDE.md", "from the project\n");
    project.Write("parent/project/.keep", "");

    const auto context = LoadProjectContext(project.dir / "parent" / "project",
                                            Settings(/*search_ancestors=*/true));
    REQUIRE(context.has_value());
    CHECK(context->file_name == "CLAUDE.md");
    CHECK(context->body == "from the project\n");
}

TEST_CASE("project context: a directory named like a candidate is not a candidate",
          "[project_context]") {
    TempProject project;
    std::filesystem::create_directories(project.dir / "AGENTS.md");

    std::string error;
    CHECK_FALSE(LoadProjectContext(project.dir, Settings(), &error).has_value());
    CHECK(error.empty());
}

TEST_CASE("RenderProjectContextSection: header names the winning file", "[project_context]") {
    ProjectContext context;
    context.body = "Be brief.\n";
    context.file_name = "CLAUDE.md";
    context.source_path = "/tmp/CLAUDE.md";

    const std::string section = RenderProjectContextSection(context);
    CHECK(section.find("## Project context") != std::string::npos);
    CHECK(section.find("Instructions from CLAUDE.md in the project root:") != std::string::npos);
    CHECK(section.find("Be brief.\n") != std::string::npos);
}

TEST_CASE("RenderProjectContextSection: a truncated body carries a visible marker",
          "[project_context]") {
    ProjectContext context;
    context.body = std::string(64, 'x');
    context.file_name = "AGENTS.md";
    context.source_path = "/tmp/AGENTS.md";
    context.truncated = true;
    context.total_bytes = 640;

    const std::string section = RenderProjectContextSection(context);
    CHECK(section.find("[... AGENTS.md truncated: showing the first 64 of 640 bytes]") !=
          std::string::npos);
}

TEST_CASE("RenderProjectContextSection: empty body renders nothing", "[project_context]") {
    ProjectContext context;
    context.body.clear();
    context.file_name = "AGENTS.md";
    CHECK(RenderProjectContextSection(context).empty());
}
