#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

#include "file_editor.hpp"
#include "fs_settings.hpp"
#include "fakes.hpp"
#include "fs_tools.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;
using minimal::tools::ToolCategory;

namespace {

struct Fixture {
    TempDir dir;
    tools_fs::FsSettings settings;

    tools::ToolContext Context() {
        tools::ToolContext ctx;
        ctx.session_id = "s1";
        ctx.project_root = dir.path();
        return ctx;
    }

    std::filesystem::path Write(const std::string& name, const std::string& content) {
        const auto path = dir.path() / name;
        std::ofstream out(path, std::ios::binary);
        out << content;
        return path;
    }

    std::string Read(const std::string& name) {
        std::ifstream in(dir.path() / name, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }
};

}  // namespace

// --- read --------------------------------------------------------------------

TEST_CASE("read renders numbered lines", "[tools_fs][read]") {
    Fixture f;
    f.Write("a.txt", "alpha\nbeta\n");
    auto tool = tools_fs::MakeReadTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"path", "a.txt"}}, ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("1\talpha") != std::string::npos);
    CHECK(result.content_text.find("2\tbeta") != std::string::npos);
}

TEST_CASE("read's footer states the total and the next offset", "[tools_fs][read]") {
    // The model has no structured channel -- if the affordance to page isn't in
    // the text, it cannot ask for the rest.
    Fixture f;
    std::string content;
    for (int i = 1; i <= 50; ++i) content += "line" + std::to_string(i) + "\n";
    f.Write("big.txt", content);
    auto tool = tools_fs::MakeReadTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"path", "big.txt"}, {"limit", 10}}, ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("of 50") != std::string::npos);
    CHECK(result.content_text.find("offset=11") != std::string::npos);
}

TEST_CASE("read reports a missing file as an error result", "[tools_fs][read]") {
    Fixture f;
    auto tool = tools_fs::MakeReadTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"path", "nope.txt"}}, ctx);
    CHECK(result.is_error);
}

TEST_CASE("read declares its path so the executor can classify it",
          "[tools_fs][confinement]") {
    Fixture f;
    auto tool = tools_fs::MakeReadTool(f.settings);

    CHECK(tool->path_confinement_supported());
    CHECK(tool->ExtractPath({{"path", "a.txt"}}) == std::optional<std::string>{"a.txt"});
    CHECK(tool->ExtractPath({{"nope", 1}}) == std::nullopt);
}

// --- write -------------------------------------------------------------------

TEST_CASE("write creates and overwrites", "[tools_fs][write]") {
    Fixture f;
    auto tool = tools_fs::MakeWriteTool(f.settings);
    auto ctx = f.Context();

    CHECK_FALSE(tool->execute({{"path", "out.txt"}, {"content", "first"}}, ctx).is_error);
    CHECK(f.Read("out.txt") == "first");

    CHECK_FALSE(tool->execute({{"path", "out.txt"}, {"content", "second"}}, ctx).is_error);
    CHECK(f.Read("out.txt") == "second");
}

TEST_CASE("write refuses a missing parent directory", "[tools_fs][write]") {
    Fixture f;
    auto tool = tools_fs::MakeWriteTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"path", "sub/dir/out.txt"}, {"content", "x"}}, ctx);

    CHECK(result.is_error);
    CHECK_FALSE(std::filesystem::exists(f.dir.path() / "sub"));
}

// --- edit --------------------------------------------------------------------

TEST_CASE("edit view reports ReadOnly so viewing never prompts to write",
          "[tools_fs][edit][invariant]") {
    Fixture f;
    auto tool = tools_fs::MakeEditTool(f.settings);

    CHECK(tool->spec().category == ToolCategory::FileWrite);
    CHECK(tool->category_for({{"command", "view"}}) == ToolCategory::ReadOnly);
    // Mutating commands fall through to the tool's own (write) category.
    CHECK(tool->category_for({{"command", "str_replace"}}) == std::nullopt);
    CHECK(tool->category_for({{"command", "insert"}}) == std::nullopt);
    CHECK(tool->category_for({{"command", "create"}}) == std::nullopt);
}

TEST_CASE("edit view honours view_range", "[tools_fs][edit]") {
    Fixture f;
    f.Write("a.txt", "one\ntwo\nthree\nfour\n");
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute(
        {{"command", "view"}, {"path", "a.txt"}, {"view_range", {2, 3}}}, ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("two") != std::string::npos);
    CHECK(result.content_text.find("three") != std::string::npos);
    CHECK(result.content_text.find("four") == std::string::npos);
}

TEST_CASE("edit create writes a new file and overwrites an existing one",
          "[tools_fs][edit]") {
    // Overwrite matches text_editor_20250728's `create`; diverging would defeat
    // the reason for adopting that vocabulary.
    Fixture f;
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    CHECK_FALSE(
        tool->execute({{"command", "create"}, {"path", "n.txt"}, {"file_text", "a"}}, ctx).is_error);
    CHECK(f.Read("n.txt") == "a");

    CHECK_FALSE(
        tool->execute({{"command", "create"}, {"path", "n.txt"}, {"file_text", "b"}}, ctx).is_error);
    CHECK(f.Read("n.txt") == "b");
}

TEST_CASE("str_replace requires exactly one match", "[tools_fs][edit][invariant]") {
    Fixture f;
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    SECTION("exactly one succeeds") {
        f.Write("a.txt", "alpha\nbeta\n");
        const auto result = tool->execute(
            {{"command", "str_replace"}, {"path", "a.txt"}, {"old_str", "beta"}, {"new_str", "GAMMA"}},
            ctx);
        CHECK_FALSE(result.is_error);
        CHECK(f.Read("a.txt") == "alpha\nGAMMA\n");
    }

    SECTION("zero matches fails and leaves the file alone") {
        f.Write("a.txt", "alpha\n");
        const auto result = tool->execute(
            {{"command", "str_replace"}, {"path", "a.txt"}, {"old_str", "nope"}, {"new_str", "x"}},
            ctx);
        CHECK(result.is_error);
        CHECK(f.Read("a.txt") == "alpha\n");
    }

    SECTION("multiple matches fails without modifying anything") {
        // The case that matters: silently replacing the first of several would
        // edit a line the caller never looked at, and the corruption is nearly
        // invisible in review.
        f.Write("a.txt", "x = 1\ny = 1\n");
        const auto result = tool->execute(
            {{"command", "str_replace"}, {"path", "a.txt"}, {"old_str", "= 1"}, {"new_str", "= 2"}},
            ctx);
        CHECK(result.is_error);
        CHECK(result.content_text.find("exactly once") != std::string::npos);
        CHECK(f.Read("a.txt") == "x = 1\ny = 1\n");
    }
}

TEST_CASE("insert places text at the requested line", "[tools_fs][edit]") {
    Fixture f;
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    SECTION("line 0 inserts before the first line") {
        f.Write("a.txt", "one\ntwo\n");
        CHECK_FALSE(tool->execute({{"command", "insert"},
                                    {"path", "a.txt"},
                                    {"insert_line", 0},
                                    {"insert_text", "zero"}},
                                   ctx)
                        .is_error);
        CHECK(f.Read("a.txt") == "zero\none\ntwo\n");
    }

    SECTION("mid-file insert") {
        f.Write("a.txt", "one\nthree\n");
        CHECK_FALSE(tool->execute({{"command", "insert"},
                                    {"path", "a.txt"},
                                    {"insert_line", 1},
                                    {"insert_text", "two"}},
                                   ctx)
                        .is_error);
        CHECK(f.Read("a.txt") == "one\ntwo\nthree\n");
    }

    SECTION("insert at the last line appends") {
        f.Write("a.txt", "one\n");
        CHECK_FALSE(tool->execute({{"command", "insert"},
                                    {"path", "a.txt"},
                                    {"insert_line", 1},
                                    {"insert_text", "two"}},
                                   ctx)
                        .is_error);
        CHECK(f.Read("a.txt") == "one\ntwo\n");
    }

    SECTION("past the end fails rather than appending") {
        // Usually means the model is working from a stale view of the file;
        // quietly appending would hide that.
        f.Write("a.txt", "one\n");
        const auto result = tool->execute(
            {{"command", "insert"}, {"path", "a.txt"}, {"insert_line", 99}, {"insert_text", "x"}},
            ctx);
        CHECK(result.is_error);
        CHECK(f.Read("a.txt") == "one\n");
    }
}

TEST_CASE("edit refuses a file over the byte cap", "[tools_fs][edit]") {
    // Mutating commands must hold the whole file to rewrite it, unlike read
    // which streams a window -- hence a bound.
    Fixture f;
    f.settings.edit_max_editable_file_bytes = 16;
    f.Write("big.txt", std::string(100, 'x'));
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute(
        {{"command", "str_replace"}, {"path", "big.txt"}, {"old_str", "x"}, {"new_str", "y"}}, ctx);

    CHECK(result.is_error);
    CHECK(result.content_text.find("limit") != std::string::npos);
}

TEST_CASE("edit rejects an unknown command", "[tools_fs][edit]") {
    Fixture f;
    f.Write("a.txt", "x\n");
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    CHECK(tool->execute({{"command", "delete_everything"}, {"path", "a.txt"}}, ctx).is_error);
}

// --- integration with the permission gate ------------------------------------

TEST_CASE("out-of-workdir access is refused by the executor, not by the tool",
          "[tools_fs][confinement][integration]") {
    // The real tools must participate in the CENTRAL classification rather than
    // each enforcing their own idea of confinement -- that is the entire reason
    // path_confinement_supported()/ExtractPath() exist on the Tool interface.
    Fixture f;
    f.Write("inside.txt", "ok\n");

    tools::ToolRegistry registry;
    registry.Register(tools_fs::MakeReadTool(f.settings));

    testing::UnreachablePermissionChecker checker;
    tools::PermissionPolicy policy;
    policy.read_only = tools::PermissionMode::AlwaysAllow;
    policy.out_of_workdir = tools::PermissionMode::AlwaysDeny;

    tools::ToolExecutor executor(registry, checker, policy, tools::McpPermissionPolicy{},
                                 tools::PermissionRunMode::Normal);
    auto ctx = f.Context();

    SECTION("a path inside the project root is allowed through") {
        const auto block = executor.Run("t1", "read", {{"path", "inside.txt"}}, ctx);
        CHECK_FALSE(block.is_error);
    }

    SECTION("a path outside it is denied before the tool ever runs") {
        const auto block = executor.Run("t2", "read", {{"path", "/etc/hosts"}}, ctx);
        CHECK(block.is_error);
        const auto& text = std::get<llm::TextBlock>(block.content.front()).text;
        // The executor's wording, not the tool's -- proving which layer decided.
        CHECK(text.find("permission denied") != std::string::npos);
    }
}

// --- settings ----------------------------------------------------------------

TEST_CASE("fs settings take defaults when absent or nonsensical", "[tools_fs][settings]") {
    CHECK(tools_fs::ParseFsSettings(nlohmann::json::object()).read_default_limit_lines == 2000);
    CHECK(tools_fs::ParseFsSettings(nlohmann::json("not an object")).read_max_line_length == 2000);

    const nlohmann::json configured = {{"read", {{"default_limit_lines", 50}}},
                                       {"edit", {{"max_editable_file_bytes", 1024}}}};
    const auto settings = tools_fs::ParseFsSettings(configured);
    CHECK(settings.read_default_limit_lines == 50);
    CHECK(settings.edit_max_editable_file_bytes == 1024);
    CHECK(settings.read_max_line_length == 2000);  // untouched key keeps its default

    // A non-positive limit would mean "read nothing", which nobody configures on
    // purpose.
    const nlohmann::json nonsense = {{"read", {{"default_limit_lines", 0}}}};
    CHECK(tools_fs::ParseFsSettings(nonsense).read_default_limit_lines == 2000);
}
