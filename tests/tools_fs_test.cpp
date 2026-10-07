#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <fstream>
#include <limits>
#include <sstream>

#include "file_editor.hpp"
#include "fs_settings.hpp"
#include "fakes.hpp"
#include "fs_tools.hpp"
#include "line_diff.hpp"
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
        tools::ToolContext ctx{common::DefaultLogger()};
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

TEST_CASE("read stops at its byte budget on a whole line and says how to continue",
          "[tools_fs][read]") {
    // 2000 lines of 2000 characters is ~4 MB: the line limits alone let one
    // read flood the context.
    Fixture f;
    f.settings.read_max_bytes = 1000;
    std::string content;
    for (int i = 1; i <= 100; ++i) content += std::string(90, 'a') + std::to_string(i) + "\n";
    f.Write("wide.txt", content);
    auto tool = tools_fs::MakeReadTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"path", "wide.txt"}}, ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("byte budget (1000 bytes)") != std::string::npos);
    CHECK(result.content_text.find("of 100. Pass offset=") != std::string::npos);
    CHECK(result.content_text.find("\t" + std::string(90, 'a') + "100") == std::string::npos);
    CHECK(result.content_text.size() < 1000 + 200);

    // One line wider than the whole budget still comes back: paging must
    // always make progress.
    f.settings.read_max_bytes = 10;
    auto narrow = tools_fs::MakeReadTool(f.settings);
    const auto one = narrow->execute({{"path", "wide.txt"}}, ctx);
    CHECK(one.content_text.find("1\t" + std::string(90, 'a') + "1\n") == 0);
    CHECK(one.content_text.find("offset=2") != std::string::npos);
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

TEST_CASE("write creates missing parent directories", "[tools_fs][write]") {
    Fixture f;
    auto tool = tools_fs::MakeWriteTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"path", "sub/dir/out.txt"}, {"content", "x"}}, ctx);

    CHECK_FALSE(result.is_error);
    CHECK(f.Read("sub/dir/out.txt") == "x");
}

TEST_CASE("edit create also creates missing parent directories", "[tools_fs][edit]") {
    Fixture f;
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute(
        {{"command", "create"}, {"path", "a/b/c.txt"}, {"file_text", "hello"}}, ctx);

    CHECK_FALSE(result.is_error);
    CHECK(f.Read("a/b/c.txt") == "hello");
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

    SECTION("overlapping matches also fail, not just adjacent ones") {
        // Regression: counting by skipping needle.size() bytes past each hit
        // missed OVERLAPPING occurrences -- "aa" in "aaa" is at offsets 0 and
        // 1, but skipping to offset 2 only ever saw the first, so the
        // uniqueness check passed and replaced a location the caller never
        // uniquely identified.
        f.Write("a.txt", "aaa\n");
        const auto result = tool->execute(
            {{"command", "str_replace"}, {"path", "a.txt"}, {"old_str", "aa"}, {"new_str", "Z"}},
            ctx);
        CHECK(result.is_error);
        CHECK(result.content_text.find("exactly once") != std::string::npos);
        CHECK(f.Read("a.txt") == "aaa\n");
    }
}

TEST_CASE("write and edit are not parallel-safe", "[tools_fs][invariant]") {
    // Regression: both default(true)d, though each is a read-modify-write of
    // the whole file with no lock -- two calls against the same path in one
    // turn race, and whichever atomic rename lands second silently discards
    // the other's change while both report success.
    Fixture f;
    CHECK_FALSE(tools_fs::MakeWriteTool(f.settings)->parallel_safe());
    CHECK_FALSE(tools_fs::MakeEditTool(f.settings)->parallel_safe());
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

    SECTION("a file with no trailing newline keeps having none") {
        // str_replace never touches the terminator; Insert used to append '\n'
        // to every line unconditionally, so this file GAINED one -- editing
        // bytes the caller never named. Fails with the fix reverted.
        f.Write("a.txt", "one\ntwo");
        CHECK_FALSE(tool->execute({{"command", "insert"},
                                    {"path", "a.txt"},
                                    {"insert_line", 1},
                                    {"insert_text", "mid"}},
                                   ctx)
                        .is_error);
        CHECK(f.Read("a.txt") == "one\nmid\ntwo");
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

TEST_CASE("str_replace refuses when the REPLACEMENT would push the file over the cap",
         "[tools_fs][edit]") {
    // The existing-file cap above bounds LoadWholeFile's input; new_str is
    // model-controlled and unbounded on its own. A small file plus a huge
    // new_str used to sail through -- the ceiling only checked what was
    // already on disk, not what execute() was about to write.
    Fixture f;
    f.settings.edit_max_editable_file_bytes = 16;
    f.Write("small.txt", "x");
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"command", "str_replace"},
                                       {"path", "small.txt"},
                                       {"old_str", "x"},
                                       {"new_str", std::string(100, 'y')}},
                                      ctx);

    CHECK(result.is_error);
    CHECK(result.content_text.find("limit") != std::string::npos);
    CHECK(f.Read("small.txt") == "x");  // rejected before the write, not after
}

TEST_CASE("insert refuses when the INSERTED text would push the file over the cap",
         "[tools_fs][edit]") {
    Fixture f;
    f.settings.edit_max_editable_file_bytes = 16;
    f.Write("small.txt", "one\n");
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"command", "insert"},
                                       {"path", "small.txt"},
                                       {"insert_line", 1},
                                       {"insert_text", std::string(100, 'y')}},
                                      ctx);

    CHECK(result.is_error);
    CHECK(result.content_text.find("limit") != std::string::npos);
    CHECK(f.Read("small.txt") == "one\n");
}

TEST_CASE("edit create refuses file_text over the byte cap, same as write",
         "[tools_fs][edit]") {
    // edit create is the OTHER whole-file-write path, alongside `write`
    // itself; it used to bypass edit_max_editable_file_bytes entirely.
    Fixture f;
    f.settings.edit_max_editable_file_bytes = 16;
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute(
        {{"command", "create"}, {"path", "new.txt"}, {"file_text", std::string(100, 'z')}}, ctx);

    CHECK(result.is_error);
    CHECK(result.content_text.find("limit") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(f.dir.path() / "new.txt"));
}

TEST_CASE("edit create over the byte cap leaves no directory debris",
         "[tools_fs][edit][invariant]") {
    // Regression: CreateParents ran BEFORE the size check, so a rejected
    // create with a nested path still left the empty parent directories
    // behind it.
    Fixture f;
    f.settings.edit_max_editable_file_bytes = 16;
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute(
        {{"command", "create"},
         {"path", "new/deep/dir/file.txt"},
         {"file_text", std::string(100, 'z')}},
        ctx);

    CHECK(result.is_error);
    CHECK_FALSE(std::filesystem::exists(f.dir.path() / "new"));
}

TEST_CASE("write over the byte cap leaves no directory debris",
         "[tools_fs][write][invariant]") {
    Fixture f;
    f.settings.edit_max_editable_file_bytes = 16;
    auto tool = tools_fs::MakeWriteTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute(
        {{"path", "new/deep/dir/file.txt"}, {"content", std::string(100, 'z')}}, ctx);

    CHECK(result.is_error);
    CHECK_FALSE(std::filesystem::exists(f.dir.path() / "new"));
}

TEST_CASE("edit rejects an unknown command", "[tools_fs][edit]") {
    Fixture f;
    f.Write("a.txt", "x\n");
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    CHECK(tool->execute({{"command", "delete_everything"}, {"path", "a.txt"}}, ctx).is_error);
}

// --- permission disclosure ----------------------------------------------------
//
// Regression: the base Tool::Disclose default names only the TOOL
// ("read"/"write"/"edit"), never the file -- so the permission modal asked
// the user to authorize a filesystem operation without saying which file,
// though the very next transcript row (post-approval) named it. Each of
// these asserts the disclosed summary names the path, and (write/edit) says
// what will happen to it.

TEST_CASE("read discloses the path", "[tools_fs][read][disclosure]") {
    Fixture f;
    auto tool = tools_fs::MakeReadTool(f.settings);
    auto ctx = f.Context();
    const auto disclosure = tool->Disclose({{"path", "src/main.cpp"}}, ctx);
    CHECK(disclosure.summary.find("src/main.cpp") != std::string::npos);
    CHECK(disclosure.complete);
}

TEST_CASE("read's disclosure is incomplete when the path is missing", "[tools_fs][read][disclosure]") {
    Fixture f;
    auto tool = tools_fs::MakeReadTool(f.settings);
    auto ctx = f.Context();
    const auto disclosure = tool->Disclose({}, ctx);
    CHECK_FALSE(disclosure.complete);
}

TEST_CASE("write discloses the path and that it creates or overwrites",
          "[tools_fs][write][disclosure]") {
    Fixture f;
    auto tool = tools_fs::MakeWriteTool(f.settings);
    auto ctx = f.Context();
    const auto disclosure = tool->Disclose({{"path", "out.txt"}, {"content", "hello"}}, ctx);
    CHECK(disclosure.summary.find("out.txt") != std::string::npos);
    CHECK(disclosure.summary.find("creates or overwrites") != std::string::npos);
    CHECK(disclosure.summary.find("5 bytes") != std::string::npos);
}

TEST_CASE("edit discloses the path and the command", "[tools_fs][edit][disclosure]") {
    Fixture f;
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();
    const auto disclosure = tool->Disclose(
        {{"command", "str_replace"}, {"path", "a.txt"}, {"old_str", "x"}, {"new_str", "y"}}, ctx);
    CHECK(disclosure.summary.find("a.txt") != std::string::npos);
    CHECK(disclosure.summary.find("str_replace") != std::string::npos);
    CHECK(disclosure.summary.find("replaces text") != std::string::npos);
}

TEST_CASE("edit create's disclosure says it creates the file", "[tools_fs][edit][disclosure]") {
    Fixture f;
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();
    const auto disclosure = tool->Disclose({{"command", "create"}, {"path", "new.txt"}}, ctx);
    CHECK(disclosure.summary.find("new.txt") != std::string::npos);
    CHECK(disclosure.summary.find("creates this file") != std::string::npos);
}

TEST_CASE("write and edit offer one project-wide edit grant, never outside or into .git",
          "[tools_fs][disclosure][grants]") {
    Fixture f;
    auto write = tools_fs::MakeWriteTool(f.settings);
    auto edit = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    const auto inside = write->Disclose({{"path", "src/a.txt"}, {"content", "x"}}, ctx);
    REQUIRE(inside.grants.size() == 1);
    CHECK(inside.grants[0].key.rfind("file_write:", 0) == 0);
    CHECK(inside.grants[0].label.find("any file in") != std::string::npos);
    // Same key from edit: one grant covers both tools.
    const auto edited = edit->Disclose(
        {{"command", "str_replace"}, {"path", "b.txt"}, {"old_str", "x"}, {"new_str", "y"}}, ctx);
    REQUIRE(edited.grants.size() == 1);
    CHECK(edited.grants[0].key == inside.grants[0].key);

    // Outside the project, and .git/ (hooks run code): nothing to grant, so
    // those calls always prompt.
    CHECK(write->Disclose({{"path", "/tmp/elsewhere.txt"}, {"content", "x"}}, ctx).grants.empty());
    CHECK(write->Disclose({{"path", ".git/hooks/pre-commit"}, {"content", "x"}}, ctx)
              .grants.empty());
    // view is read-only: it never asks, so it offers nothing.
    CHECK(edit->Disclose({{"command", "view"}, {"path", "b.txt"}}, ctx).grants.empty());
}

TEST_CASE("disclosure names the destination a symlink hides", "[tools_fs][disclosure]") {
    // The Ask prompt shows the path as the model typed it; a symlink planted
    // in an earlier round must not let the operator consent to "notes" while
    // the write lands in ~/.ssh. The canonical destination is appended
    // whenever it differs from the typed form -- and ONLY then, so ordinary
    // prompts stay clean.
    Fixture f;
    f.Write("real.txt", "x");
    std::error_code ec;
    std::filesystem::create_symlink("real.txt", f.dir.path() / "link.txt", ec);
    REQUIRE_FALSE(ec);

    auto tool = tools_fs::MakeReadTool(f.settings);
    auto ctx = f.Context();

    const auto disclosed = tool->Disclose({{"path", "link.txt"}}, ctx);
    CHECK(disclosed.summary.find("link.txt") != std::string::npos);
    CHECK(disclosed.summary.find("resolves to") != std::string::npos);
    CHECK(disclosed.summary.find("real.txt") != std::string::npos);

    const auto plain = tool->Disclose({{"path", "real.txt"}}, ctx);
    CHECK(plain.summary.find("resolves to") == std::string::npos);
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

    tools::ToolExecutor executor(registry, checker, policy, tools::PermissionModels{},
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

// --- display payloads --------------------------------------------------------

namespace {
nlohmann::json DiffLine(const char* op, const char* text) {
    return nlohmann::json{{"op", op}, {"text", text}};
}
std::string NumberedLines(int count, const char* prefix = "l") {
    std::string text;
    for (int i = 1; i <= count; ++i) text += prefix + std::to_string(i) + "\n";
    return text;
}
}  // namespace

TEST_CASE("DiffDisplay: one changed line is one hunk with real line numbers and 3 lines of "
          "context",
          "[tools_fs][display]") {
    const std::string before = NumberedLines(10);
    std::string after = before;
    after.replace(after.find("l5\n"), 3, "L5\n");

    const auto display = tools_fs::DiffDisplay("a.txt", before, after, false);

    REQUIRE(display.at("kind") == "diff");
    CHECK(display.at("path") == "a.txt");
    CHECK(display.at("created") == false);
    REQUIRE(display.at("hunks").size() == 1);
    const auto& hunk = display.at("hunks")[0];
    CHECK(hunk.at("old_start") == 2);
    CHECK(hunk.at("old_count") == 7);
    CHECK(hunk.at("new_start") == 2);
    CHECK(hunk.at("new_count") == 7);
    REQUIRE(hunk.at("lines").size() == 8);   // l2-l4, -l5, +L5, l6-l8
    CHECK(hunk.at("lines")[0] == DiffLine(" ", "l2"));
    CHECK(hunk.at("lines")[3] == DiffLine("-", "l5"));
    CHECK(hunk.at("lines")[4] == DiffLine("+", "L5"));
    CHECK(hunk.at("lines")[7] == DiffLine(" ", "l8"));
}

TEST_CASE("DiffDisplay: distant changes are separate hunks, near ones merge",
          "[tools_fs][display]") {
    const std::string before = NumberedLines(30);
    std::string distant = before;
    distant.replace(distant.find("l2\n"), 3, "X\n");
    distant.replace(distant.find("l28\n"), 4, "Y\n");
    CHECK(tools_fs::DiffDisplay("a", before, distant, false).at("hunks").size() == 2);

    std::string near = before;   // 6 unchanged lines apart: the contexts touch
    near.replace(near.find("l10\n"), 4, "X\n");
    near.replace(near.find("l17\n"), 4, "Y\n");
    CHECK(tools_fs::DiffDisplay("a", before, near, false).at("hunks").size() == 1);
}

TEST_CASE("DiffDisplay: a created file is one all-added hunk starting at line 1",
          "[tools_fs][display]") {
    const auto display = tools_fs::DiffDisplay("n.txt", "", "a\nb\n", true);
    CHECK(display.at("created") == true);
    REQUIRE(display.at("hunks").size() == 1);
    const auto& hunk = display.at("hunks")[0];
    CHECK(hunk.at("old_start") == 0);
    CHECK(hunk.at("old_count") == 0);
    CHECK(hunk.at("new_start") == 1);
    CHECK(hunk.at("new_count") == 2);
    CHECK(hunk.at("lines")[1] == DiffLine("+", "b"));
}

TEST_CASE("DiffDisplay: no change, binary content, and oversized changes", "[tools_fs][display]") {
    CHECK(tools_fs::DiffDisplay("a", "same\n", "same\n", false).at("hunks").empty());
    CHECK(tools_fs::DiffDisplay("a", std::string("a\0b", 3), "a\n", false).is_null());
    // Too large to review as a diff: no payload, the client shows the text.
    CHECK(tools_fs::DiffDisplay("a", "", NumberedLines(3000), true).is_null());
}

TEST_CASE("DiffDisplay: a line with no trailing newline diffs as a line", "[tools_fs][display]") {
    const auto display = tools_fs::DiffDisplay("a", "one\ntwo", "one\nTWO", false);
    const auto& lines = display.at("hunks")[0].at("lines");
    CHECK(lines[1] == DiffLine("-", "two"));
    CHECK(lines[2] == DiffLine("+", "TWO"));
}

TEST_CASE("edit str_replace and insert return a diff payload naming the typed path",
          "[tools_fs][display]") {
    Fixture f;
    f.Write("a.txt", "one\ntwo\nthree\n");
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    const auto replaced = tool->execute(
        {{"command", "str_replace"}, {"path", "a.txt"}, {"old_str", "two"}, {"new_str", "2"}},
        ctx);
    REQUIRE_FALSE(replaced.is_error);
    CHECK(replaced.content_text == "replaced 1 occurrence in a.txt");   // the model's view
    REQUIRE(replaced.display.at("kind") == "diff");
    CHECK(replaced.display.at("path") == "a.txt");
    CHECK(replaced.display.at("hunks")[0].at("lines")[1] == DiffLine("-", "two"));
    CHECK(replaced.display.at("hunks")[0].at("lines")[2] == DiffLine("+", "2"));

    const auto inserted = tool->execute(
        {{"command", "insert"}, {"path", "a.txt"}, {"insert_line", 1}, {"insert_text", "new"}},
        ctx);
    REQUIRE_FALSE(inserted.is_error);
    const auto& hunk = inserted.display.at("hunks")[0];
    CHECK(hunk.at("new_start") == 1);
    CHECK(hunk.at("lines")[1] == DiffLine("+", "new"));
}

TEST_CASE("write diffs an overwrite against the previous content; a new file is created",
          "[tools_fs][display]") {
    Fixture f;
    auto tool = tools_fs::MakeWriteTool(f.settings);
    auto ctx = f.Context();

    const auto created = tool->execute({{"path", "n.txt"}, {"content", "x\n"}}, ctx);
    REQUIRE_FALSE(created.is_error);
    CHECK(created.display.at("created") == true);

    const auto overwritten = tool->execute({{"path", "n.txt"}, {"content", "y\n"}}, ctx);
    REQUIRE_FALSE(overwritten.is_error);
    CHECK(overwritten.display.at("created") == false);
    const auto& lines = overwritten.display.at("hunks")[0].at("lines");
    CHECK(lines[0] == DiffLine("-", "x"));
    CHECK(lines[1] == DiffLine("+", "y"));
}

TEST_CASE("read returns a file payload with raw, unnumbered lines", "[tools_fs][display]") {
    Fixture f;
    f.Write("a.py", "alpha\nbeta\n");
    auto tool = tools_fs::MakeReadTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"path", "a.py"}}, ctx);

    REQUIRE(result.display.at("kind") == "file");
    CHECK(result.display.at("path") == "a.py");
    CHECK(result.display.at("start_line") == 1);
    CHECK(result.display.at("end_line") == 2);
    CHECK(result.display.at("total_lines") == 2);
    CHECK(result.display.at("truncated") == false);
    CHECK(result.display.at("lines") == nlohmann::json::array({"alpha", "beta"}));
}

// --- settings ----------------------------------------------------------------

TEST_CASE("fs settings: defaults, overrides, and named refusals", "[tools_fs][settings]") {
    CHECK(tools_fs::ParseFsSettings(nlohmann::json::object()).read_default_limit_lines == 2000);

    const nlohmann::json configured = {{"read", {{"default_limit_lines", 50}}},
                                       {"edit", {{"max_editable_file_bytes", 1024}}}};
    const auto settings = tools_fs::ParseFsSettings(configured);
    CHECK(settings.read_default_limit_lines == 50);
    CHECK(settings.edit_max_editable_file_bytes == 1024);
    CHECK(settings.read_max_line_length == 2000);  // untouched key keeps its default

    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(tools_fs::ParseFsSettings(nlohmann::json("not an object")),
                      ContainsSubstring("plugins.d/tools-fs.json: the top level: must be an object"));
    // A non-positive limit would mean "read nothing", which nobody configures
    // on purpose.
    CHECK_THROWS_WITH(tools_fs::ParseFsSettings({{"read", {{"default_limit_lines", 0}}}}),
                      ContainsSubstring("read.default_limit_lines: must be at least 1"));
    CHECK_THROWS_WITH(tools_fs::ParseFsSettings({{"read", {{"max_line", 10}}}}),
                      ContainsSubstring("read.max_line: unknown field"));
}

TEST_CASE("reading one enormous line does not allocate the whole file",
          "[tools_fs][read][invariant]") {
    // ARCHITECTURE.md promises "memory proportional to `limit`, not file size".
    // std::getline materialized the WHOLE line before anything truncated it, so
    // a minified file with no newline -- and no NUL, so the binary probe passes
    // -- was a full-file allocation to display a couple of thousand characters.
    Fixture f;
    const std::size_t kHuge = 40u * 1024 * 1024;  // 40 MB on one line
    const auto path = f.dir.path() / "minified.js";
    {
        std::ofstream out(path, std::ios::binary);
        const std::string block(1u << 20, 'a');
        for (int i = 0; i < 40; ++i) out << block;
        out << "\n";
    }
    REQUIRE(std::filesystem::file_size(path) > kHuge);

    const auto window = tools::read_window(path, 1, 10, /*max_line_length=*/2000);
    REQUIRE(window.ok);
    REQUIRE(window.lines.size() == 1);
    // Truncated to the configured width plus the marker -- never the 40 MB.
    CHECK(window.lines.front().size() < 2100);
    CHECK(window.total_lines == 1);
}

TEST_CASE("a window past the end of a file is empty, not an overflow",
          "[tools_fs][read]") {
    // offset and limit are model-written: `offset + limit - 1` overflowed for
    // values near INT_MAX, and signed overflow is UB rather than a large number.
    Fixture f;
    const auto path = f.dir.path() / "small.txt";
    std::ofstream(path) << "one\ntwo\nthree\n";

    const auto window = tools::read_window(path, 1, std::numeric_limits<int>::max(), 2000);
    REQUIRE(window.ok);
    CHECK(window.lines.size() == 3);
    CHECK_FALSE(window.truncated);
}

TEST_CASE("a NUL past the binary sniff truncates its line with a visible marker",
          "[tools_fs][read][invariant]") {
    // A file that is text in the first 8 KiB but carries a NUL deeper in:
    // strlen stopped at it, so the model was handed a line ending mid-content
    // with nothing saying so -- a corrupted view indistinguishable from a real
    // short line. The marker matches the overlong-line footer's voice.
    Fixture f;
    const auto path = f.dir.path() / "mostly-text.log";
    {
        std::ofstream out(path, std::ios::binary);
        out << "entry before\n";
        for (int i = 0; i < 3000; ++i) out << "pad\n";  // push past the 8 KiB sniff window
        out << "short line" << '\0' << "hidden tail\n";
        out << "entry last\n";
    }

    const auto window = tools::read_window(path, 1, 0 /* no limit */, 2000);
    REQUIRE(window.ok);
    REQUIRE(window.lines.size() == 3003);
    // The NUL line carries the marker; the bytes past the NUL are not rendered
    // as if they were a normal continuation.
    const std::string& nul_line = window.lines[3001];
    CHECK(nul_line.find("NUL byte") != std::string::npos);
    CHECK(nul_line.find("hidden tail") == std::string::npos);
    CHECK(nul_line.substr(0, 10) == "short line");
    // Real lines carry no marker.
    CHECK(window.lines[0] == "entry before");
    CHECK(window.lines[1] == "pad");
    CHECK(window.lines[3002] == "entry last");
}

TEST_CASE("read windows are clamped to the configured ceiling; the clamp is disclosed",
          "[tools_fs][invariant]") {
    // "Memory proportional to `limit`, not file size" was hollow when `limit`
    // was model-controlled: `{"limit": 0}` meant the WHOLE FILE and a 10 GB
    // log was an OOM on the session worker. Everything the model sends is
    // clamped to `read.max_lines`, `limit: 0` included -- and the clamp is
    // disclosed in the footer, because a clipped window presented as the
    // whole file is the false-negative failure shape.
    tools_fs::FsSettings settings;  // default ceiling: 2000
    settings.read_max_lines = 5;
    TempDir dir;
    std::ofstream(dir.path() / "big.txt") << "one\ntwo\nthree\nfour\nfive\nsix\nseven\n";

    auto tool = tools_fs::MakeReadTool(settings);
    tools::ToolContext ctx{common::DefaultLogger()};
    ctx.session_id = "s1";
    ctx.project_root = dir.path();

    // limit: 0 -- the old "whole file" -- is the ceiling now.
    auto result = tool->execute({{"path", "big.txt"}, {"limit", 0}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("[The requested window was clipped") != std::string::npos);
    CHECK(result.content_text.find("six") == std::string::npos);  // clipped at 5 lines

    // A model-authored huge limit is clamped the same way.
    result = tool->execute({{"path", "big.txt"}, {"limit", 2000000000}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("[The requested window was clipped") != std::string::npos);

    // Out-of-range input is the model's mistake to see, not a silent wrap.
    result = tool->execute({{"path", "big.txt"}, {"offset", 3000000000}}, ctx);
    CHECK(result.is_error);
}

// --- review wave: CRLF, view_range -1, disclosure, settings, diff ------------

TEST_CASE("insert into a CRLF file writes CRLF, never a bare LF line", "[tools_fs][edit]") {
    Fixture f;
    f.Write("w.txt", "one\r\ntwo\r\n");
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"command", "insert"}, {"path", "w.txt"},
                                       {"insert_line", 1}, {"insert_text", "a\nb\n"}},
                                      ctx);
    REQUIRE_FALSE(result.is_error);
    CHECK(f.Read("w.txt") == "one\r\na\r\nb\r\ntwo\r\n");

    // A mixed file is not normalized: only the inserted line is new bytes.
    f.Write("m.txt", "one\r\ntwo\n");
    REQUIRE_FALSE(tool->execute({{"command", "insert"}, {"path", "m.txt"}, {"insert_line", 0},
                                 {"insert_text", "zero"}},
                                ctx)
                      .is_error);
    CHECK(f.Read("m.txt") == "zero\none\r\ntwo\n");
}

TEST_CASE("str_replace matches an LF old_str in a CRLF file and keeps CRLF",
          "[tools_fs][edit]") {
    Fixture f;
    f.Write("w.txt", "alpha\r\nbeta\r\ngamma\r\n");
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"command", "str_replace"}, {"path", "w.txt"},
                                       {"old_str", "alpha\nbeta"}, {"new_str", "A\nB"}},
                                      ctx);
    REQUIRE_FALSE(result.is_error);
    CHECK(f.Read("w.txt") == "A\r\nB\r\ngamma\r\n");

    // An exact match is taken as-is, bytes untouched by any translation.
    REQUIRE_FALSE(tool->execute({{"command", "str_replace"}, {"path", "w.txt"},
                                 {"old_str", "gamma"}, {"new_str", "g\nG"}},
                                ctx)
                      .is_error);
    CHECK(f.Read("w.txt") == "A\r\nB\r\ng\nG\r\n");
}

TEST_CASE("edit view accepts end -1 as 'to the end of the file'", "[tools_fs][edit]") {
    Fixture f;
    f.Write("a.txt", "one\ntwo\nthree\nfour\n");
    auto tool = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    const auto result =
        tool->execute({{"command", "view"}, {"path", "a.txt"}, {"view_range", {3, -1}}}, ctx);
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content_text.find("two") == std::string::npos);
    CHECK(result.content_text.find("3\tthree") != std::string::npos);
    CHECK(result.content_text.find("4\tfour") != std::string::npos);

    CHECK(tool->execute({{"command", "view"}, {"path", "a.txt"}, {"view_range", {1, -2}}}, ctx)
              .is_error);
}

TEST_CASE("disclosure does not call a cosmetic ./ or // spelling a redirect",
          "[tools_fs][disclosure]") {
    Fixture f;
    std::filesystem::create_directories(f.dir.path() / "sub");
    f.Write("sub/a.txt", "x");
    auto tool = tools_fs::MakeReadTool(f.settings);
    auto ctx = f.Context();

    CHECK(tool->Disclose({{"path", "./sub/a.txt"}}, ctx).summary.find("resolves to") ==
          std::string::npos);
    CHECK(tool->Disclose({{"path", "sub//a.txt"}}, ctx).summary.find("resolves to") ==
          std::string::npos);
    // A new file (write) resolves the same way.
    auto write = tools_fs::MakeWriteTool(f.settings);
    CHECK(write->Disclose({{"path", "./sub/new.txt"}, {"content", "x"}}, ctx)
              .summary.find("resolves to") == std::string::npos);
}

TEST_CASE("a default read under a smaller ceiling is not reported as clipped",
          "[tools_fs][read]") {
    // The default window is not something the model asked for; clipping it to
    // the ceiling is configuration, and the truncation footer already says
    // how to page on.
    Fixture f;
    f.settings.read_max_lines = 2;  // default_limit_lines stays 2000
    f.Write("a.txt", "one\ntwo\nthree\n");
    auto tool = tools_fs::MakeReadTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"path", "a.txt"}}, ctx);
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content_text.find("three") == std::string::npos);
    CHECK(result.content_text.find("clipped") == std::string::npos);
    CHECK(result.content_text.find("Pass offset=3") != std::string::npos);
}

TEST_CASE("fs settings refuse huge, fractional and contradictory values",
          "[tools_fs][settings]") {
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(tools_fs::ParseFsSettings({{"read", {{"max_lines", 5000000000LL}}}}),
                      ContainsSubstring("read.max_lines: must be at most"));
    CHECK_THROWS_WITH(
        tools_fs::ParseFsSettings({{"read", {{"default_limit_lines", 500}, {"max_lines", 100}}}}),
        ContainsSubstring("read.default_limit_lines: must not exceed read.max_lines"));
    CHECK_THROWS_WITH(tools_fs::ParseFsSettings({{"read", {{"max_lines", 99.5}}}}),
                      ContainsSubstring("read.max_lines: must be an integer"));
}

TEST_CASE("write and edit results name the path as typed", "[tools_fs][write][edit]") {
    Fixture f;
    auto write = tools_fs::MakeWriteTool(f.settings);
    auto edit = tools_fs::MakeEditTool(f.settings);
    auto ctx = f.Context();

    CHECK(write->execute({{"path", "sub/w.txt"}, {"content", "abc"}}, ctx).content_text ==
          "Created sub/w.txt (3 bytes)");
    CHECK(write->execute({{"path", "sub/w.txt"}, {"content", "ab"}}, ctx).content_text ==
          "Wrote sub/w.txt (2 bytes)");
    CHECK(edit->execute({{"command", "create"}, {"path", "sub/w.txt"}, {"file_text", "a"}}, ctx)
              .content_text == "Overwrote sub/w.txt (1 bytes)");
    CHECK(edit->execute({{"command", "insert"}, {"path", "sub/w.txt"}, {"insert_line", 1},
                         {"insert_text", "b"}},
                        ctx)
              .content_text == "inserted after line 1 in sub/w.txt");
}

TEST_CASE("DiffDisplay: a change to the final newline alone is a visible change",
          "[tools_fs][display]") {
    const auto added = tools_fs::DiffDisplay("a", "one\ntwo", "one\ntwo\n", false);
    REQUIRE(added.at("hunks").size() == 1);
    const auto& lines = added.at("hunks")[0].at("lines");
    CHECK(lines[1] == nlohmann::json({{"op", "-"}, {"text", "two"}}));
    CHECK(lines[2] == nlohmann::json({{"op", "+"}, {"text", "two"}}));

    CHECK(tools_fs::DiffDisplay("a", "one\n", "one\n", false).at("hunks").empty());
}
