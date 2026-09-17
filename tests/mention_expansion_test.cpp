// Tier 3: expansion against a real temp project_root -- real confinement, real
// read_window, real directory listings. The wire path (a turn whose persisted
// message carries the blocks) is covered in session_service_test.cpp.
#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "minimal/agent/mention_expansion.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;

namespace {

void WriteFile(const TempDir& dir, const std::string& name, const std::string& content) {
    const auto path = dir.path() / name;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

std::string BlockText(const llm::Message& message, std::size_t index) {
    REQUIRE(message.role == llm::Role::User);
    REQUIRE(index < message.content.size());
    return std::get<llm::TextBlock>(message.content[index]).text;
}

}  // namespace

TEST_CASE("no mentions: byte-identical to the plain TextBlock wrap", "[mention_expansion][invariant]") {
    TempDir dir;
    const std::string text = "just a normal sentence, no @ anywhere";

    const auto message = agent::ExpandMentions(text, dir.path(), common::DefaultLogger());

    // This invariant is what makes the unconditional call in Session::RunOneTurn
    // safe. Proven load-bearing: any change that adds a block or alters the text
    // for mention-free input fails here.
    REQUIRE(message.role == llm::Role::User);
    REQUIRE(message.content.size() == 1);
    CHECK(BlockText(message, 0) == text);
}

TEST_CASE("one file mention: verbatim text plus a framed block", "[mention_expansion]") {
    TempDir dir;
    WriteFile(dir, "notes.md", "hello contents");

    const auto message = agent::ExpandMentions("look at @notes.md please", dir.path(),
                                               common::DefaultLogger());

    // Block 0 is the ORIGINAL text -- tokens intact, nothing replaced in place.
    REQUIRE(message.content.size() == 2);
    CHECK(BlockText(message, 0) == "look at @notes.md please");
    CHECK(BlockText(message, 1) == "<file path=\"notes.md\">\nhello contents\n</file>");
}

TEST_CASE("several mentions become several blocks, independently resolved",
          "[mention_expansion]") {
    TempDir dir;
    WriteFile(dir, "a.md", "A");
    WriteFile(dir, "sub/b.md", "B");

    const auto message = agent::ExpandMentions("see @a.md and @sub/b.md", dir.path(),
                                               common::DefaultLogger());

    REQUIRE(message.content.size() == 3);
    CHECK(BlockText(message, 0) == "see @a.md and @sub/b.md");
    CHECK(BlockText(message, 1) == "<file path=\"a.md\">\nA\n</file>");
    CHECK(BlockText(message, 2) == "<file path=\"sub/b.md\">\nB\n</file>");
}

TEST_CASE("the same file twice in one message is read twice, unduplicated",
          "[mention_expansion]") {
    TempDir dir;
    WriteFile(dir, "a.md", "A");

    const auto message = agent::ExpandMentions("@a.md and again @a.md", dir.path(),
                                               common::DefaultLogger());

    // A mention means "read this now"; there is no dedup, and none wanted --
    // re-mentioning mid-conversation picks up edits.
    REQUIRE(message.content.size() == 3);
    CHECK(BlockText(message, 1) == BlockText(message, 2));
}

TEST_CASE("an oversize file is capped with a visible note, never split",
          "[mention_expansion]") {
    TempDir dir;
    std::string content;
    for (int i = 1; i <= 1500; ++i) content += "line" + std::to_string(i) + "\n";
    WriteFile(dir, "big.txt", content);

    const auto message = agent::ExpandMentions("@big.txt", dir.path(), common::DefaultLogger());

    REQUIRE(message.content.size() == 2);
    const auto text = BlockText(message, 1);
    CHECK(text.find("<file path=\"big.txt\">") == 0);
    CHECK(text.find("line1000\n") != std::string::npos);
    CHECK(text.find("line1001\n") == std::string::npos);
    CHECK(text.find("[truncated: showing lines 1-1000 of 1500 -- use the read tool for more]") !=
          std::string::npos);
    CHECK(text.ends_with("</file>"));
}

TEST_CASE("an unresolvable mention stays verbatim and contributes no block",
          "[mention_expansion]") {
    TempDir dir;
    WriteFile(dir, "real.txt", "x");

    const auto message = agent::ExpandMentions("see @missing.txt and @real.txt", dir.path(),
                                               common::DefaultLogger());

    // No error channel: the model sees the token and can tell the user.
    REQUIRE(message.content.size() == 2);
    CHECK(BlockText(message, 0) == "see @missing.txt and @real.txt");
    CHECK(BlockText(message, 1) == "<file path=\"real.txt\">\nx\n</file>");
}

TEST_CASE("a binary file stays verbatim", "[mention_expansion]") {
    TempDir dir;
    std::string content = "text prefix";
    content.push_back('\0');  // NUL within the first 8 KiB: read_window's binary test
    content += "binary payload";
    WriteFile(dir, "blob.bin", content);

    const auto message = agent::ExpandMentions("@blob.bin", dir.path(), common::DefaultLogger());

    REQUIRE(message.content.size() == 1);
    CHECK(BlockText(message, 0) == "@blob.bin");
}

TEST_CASE("a path reaching outside project_root stays verbatim", "[mention_expansion]") {
    TempDir project;
    TempDir outside;
    WriteFile(outside, "secret.txt", "typed by the human");

    const auto message = agent::ExpandMentions(
        "look at @../" + outside.path().filename().string() + "/secret.txt", project.path(),
        common::DefaultLogger());

    // The read tool would raise an Ask for the same path, so inlining it
    // silently would make a mention a side channel around the permission
    // system: the model can request the out-of-root read through the gated
    // tool, where it belongs. Left verbatim, nothing is disclosed.
    REQUIRE(message.content.size() == 1);
    CHECK(BlockText(message, 0).find("secret.txt") != std::string::npos);
}

TEST_CASE("mentions beyond the per-message cap stay verbatim", "[mention_expansion]") {
    TempDir dir;
    std::string text;
    for (int i = 0; i < 40; ++i) {
        WriteFile(dir, "f" + std::to_string(i) + ".txt", "content " + std::to_string(i));
        text += (i == 0 ? "" : " and ") + std::string("@f") + std::to_string(i) + ".txt";
    }

    const auto message = agent::ExpandMentions(text, dir.path(), common::DefaultLogger());

    // 1 original text block + one framed block per expanded mention, capped:
    // the rest stay verbatim inside the original block instead of N x ~2 MiB
    // of unbounded prompt growth.
    REQUIRE(message.content.size() == 33);
    for (std::size_t i = 0; i < message.content.size(); ++i) {
        (void)i;
    }
    CHECK(BlockText(message, 32) == "<file path=\"f31.txt\">\ncontent 31\n</file>");
}

TEST_CASE("a mid-word @ is prose, not a mention", "[mention_expansion]") {
    TempDir dir;
    WriteFile(dir, "b.txt", "x");

    // The mid-word token must point at something that WOULD resolve, or the
    // case cannot discriminate: "@b.com" would stay verbatim either way. Here
    // "a@b.txt" mid-word would expand if the boundary rule were dropped.
    const auto message = agent::ExpandMentions("file a@b.txt and @b.txt",
                                               dir.path(), common::DefaultLogger());

    // Proven load-bearing: dropping the whitespace-boundary rule makes the
    // scanner tokenize "a@b.txt" too, producing a third block -- this case fails.
    REQUIRE(message.content.size() == 2);
    CHECK(BlockText(message, 0) == "file a@b.txt and @b.txt");
    CHECK(BlockText(message, 1) == "<file path=\"b.txt\">\nx\n</file>");
}

TEST_CASE("a bare @ is not a token", "[mention_expansion]") {
    TempDir dir;

    const auto message = agent::ExpandMentions("call me @ or email a@b", dir.path(),
                                               common::DefaultLogger());

    REQUIRE(message.content.size() == 1);
    CHECK(BlockText(message, 0) == "call me @ or email a@b");
}

TEST_CASE("a directory mention lists a bounded tree", "[mention_expansion]") {
    TempDir dir;
    WriteFile(dir, "src/main.cpp", "x");
    WriteFile(dir, "src/util/helper.cpp", "y");
    WriteFile(dir, "src/.hidden", "z");

    const auto message = agent::ExpandMentions("structure of @src", dir.path(),
                                               common::DefaultLogger());

    REQUIRE(message.content.size() == 2);
    const auto text = BlockText(message, 1);
    CHECK(text.starts_with("<file-tree path=\"src\">"));
    // Project-root-relative paths, one per line, sorted, directories suffixed,
    // dotfiles skipped -- so a listing line is a literal read argument. The
    // always-on first-line note makes the frame self-describing about that.
    CHECK(text.find("[paths are relative to the project root -- pass them to read as-is]\n") !=
          std::string::npos);
    CHECK(text.find("src/main.cpp\n") != std::string::npos);
    CHECK(text.find("src/util/\n") != std::string::npos);
    CHECK(text.find("src/util/helper.cpp\n") != std::string::npos);
    CHECK(text.find(".hidden") == std::string::npos);
    CHECK(text.ends_with("</file-tree>"));
}

TEST_CASE("directory mention spellings that name the same directory list identically",
          "[mention_expansion]") {
    TempDir dir;
    WriteFile(dir, "src/main.cpp", "x");

    const auto plain = agent::ExpandMentions("@src", dir.path(), common::DefaultLogger());
    const auto slashed = agent::ExpandMentions("@src/", dir.path(), common::DefaultLogger());
    const auto dotted = agent::ExpandMentions("@./src", dir.path(), common::DefaultLogger());
    REQUIRE(plain.content.size() == 2);
    REQUIRE(slashed.content.size() == 2);
    REQUIRE(dotted.content.size() == 2);

    // The attribute stays as typed (it differs across spellings), but the
    // LISTING is canonical: same directory -> identical lines.
    const auto listing = [](const std::string& frame) {
        static const std::string note =
            "[paths are relative to the project root -- pass them to read as-is]\n";
        const auto pos = frame.find(note);
        REQUIRE(pos != std::string::npos);
        return frame.substr(pos + note.size());
    };
    const auto plain_listing = listing(BlockText(plain, 1));
    CHECK(plain_listing.find("src/main.cpp\n") != std::string::npos);
    CHECK(listing(BlockText(slashed, 1)) == plain_listing);
    CHECK(listing(BlockText(dotted, 1)) == plain_listing);
}

TEST_CASE("a root mention lists bare names and still carries the note", "[mention_expansion]") {
    TempDir dir;
    WriteFile(dir, "top.txt", "x");
    WriteFile(dir, "sub/inner.txt", "y");

    const auto message = agent::ExpandMentions("everything in @.", dir.path(),
                                               common::DefaultLogger());

    REQUIRE(message.content.size() == 2);
    const auto text = BlockText(message, 1);
    CHECK(text.starts_with("<file-tree path=\".\">"));
    // The root itself: no prefix to add, so bare names are already correct.
    CHECK(text.find("[paths are relative to the project root -- pass them to read as-is]\n") !=
          std::string::npos);
    CHECK(text.find("\ntop.txt\n") != std::string::npos);
    CHECK(text.find("sub/\n") != std::string::npos);
    CHECK(text.find("./top.txt") == std::string::npos);
}

TEST_CASE("the directory tree respects its depth bound", "[mention_expansion]") {
    TempDir dir;
    WriteFile(dir, "top/level1/level2/level3/deep.txt", "x");

    const auto message = agent::ExpandMentions("@top", dir.path(), common::DefaultLogger());

    REQUIRE(message.content.size() == 2);
    const auto text = BlockText(message, 1);
    // Three levels below the mention are listed (with the root-relative
    // prefix), but level3 is never recursed into -- its children are absent.
    CHECK(text.find("top/level1/\n") != std::string::npos);
    CHECK(text.find("top/level1/level2/level3/\n") != std::string::npos);
    CHECK(text.find("deep.txt") == std::string::npos);
}

TEST_CASE("a non-regular, non-directory mention stays verbatim", "[mention_expansion]") {
    TempDir dir;
    const auto fifo = dir.path() / "pipe.fifo";
    REQUIRE(::mkfifo(fifo.c_str(), 0644) == 0);

    // read_window on a FIFO would block the session worker on open; a mention
    // must never wedge a turn, so anything that is neither directory nor
    // regular file is treated as unresolvable.
    const auto message = agent::ExpandMentions("@pipe.fifo", dir.path(), common::DefaultLogger());

    REQUIRE(message.content.size() == 1);
    CHECK(BlockText(message, 0) == "@pipe.fifo");
}

TEST_CASE("empty text passes through untouched", "[mention_expansion]") {
    TempDir dir;

    const auto message = agent::ExpandMentions("", dir.path(), common::DefaultLogger());

    REQUIRE(message.content.size() == 1);
    CHECK(BlockText(message, 0).empty());
}
