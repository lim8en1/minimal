// Pure logic, tier 1: the suggestion strategy. Listing rules live here; the
// wire surface (SuggestPaths) is exercised in session_service_test.cpp.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "minimal/agent/mention_resolver.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;

namespace {

void MakeDir(const TempDir& dir, const std::string& name) {
    std::filesystem::create_directories(dir.path() / name);
}

void WriteFile(const TempDir& dir, const std::string& name, const std::string& content = "") {
    std::ofstream out(dir.path() / name);
    out << content;
}

agent::PrefixMentionResolver Resolver() { return {}; }

}  // namespace

TEST_CASE("lists root entries matching the fragment", "[mention_resolver]") {
    TempDir dir;
    MakeDir(dir, "src");
    MakeDir(dir, "tests");
    WriteFile(dir, "README.md");

    const auto paths = Resolver().Suggest("s", dir.path(), 50);

    // Directories carry the '/' the client needs to keep typing into them.
    CHECK(paths == std::vector<std::string>{"src/"});
}

TEST_CASE("an empty fragment lists the project root", "[mention_resolver]") {
    TempDir dir;
    MakeDir(dir, "build");
    WriteFile(dir, "main.cpp");

    const auto paths = Resolver().Suggest("", dir.path(), 50);

    CHECK(paths == std::vector<std::string>{"build/", "main.cpp"});
}

TEST_CASE("descends only on '/' -- the load-bearing rule", "[mention_resolver]") {
    TempDir dir;
    MakeDir(dir, "src");
    MakeDir(dir, "src/utils");
    MakeDir(dir, "srcdocs");
    WriteFile(dir, "src/main.cpp");

    // "src" with no slash matches root entries ONLY -- src/ and srcdocs/. It
    // must NOT reach into src/ for src/main.cpp. Proven load-bearing: making
    // Suggest fall through to the subdirectory's contents fails this case.
    CHECK(Resolver().Suggest("src", dir.path(), 50) ==
          std::vector<std::string>{"src/", "srcdocs/"});

    // The slash is what authorizes descent, and results are relative to
    // project_root INCLUDING the typed prefix -- what a client appends after
    // '@' to make a complete reference.
    CHECK(Resolver().Suggest("src/", dir.path(), 50) ==
          std::vector<std::string>{"src/main.cpp", "src/utils/"});
    CHECK(Resolver().Suggest("src/ma", dir.path(), 50) ==
          std::vector<std::string>{"src/main.cpp"});
}

TEST_CASE("the deepest complete directory wins", "[mention_resolver]") {
    TempDir dir;
    MakeDir(dir, "a/b");
    WriteFile(dir, "a/b/leaf.txt");

    const auto paths = Resolver().Suggest("a/b/le", dir.path(), 50);

    CHECK(paths == std::vector<std::string>{"a/b/leaf.txt"});
}

TEST_CASE("prefix matching is case-sensitive", "[mention_resolver]") {
    TempDir dir;
    WriteFile(dir, "README.md");

    CHECK(Resolver().Suggest("read", dir.path(), 50).empty());
    CHECK(Resolver().Suggest("READ", dir.path(), 50) == std::vector<std::string>{"README.md"});
}

TEST_CASE("dotfiles are invisible unless the fragment asks", "[mention_resolver]") {
    TempDir dir;
    WriteFile(dir, ".gitignore");
    WriteFile(dir, "main.cpp");

    CHECK(Resolver().Suggest("", dir.path(), 50) == std::vector<std::string>{"main.cpp"});
    CHECK(Resolver().Suggest(".", dir.path(), 50) == std::vector<std::string>{".gitignore"});
}

TEST_CASE("a nonexistent directory yields an empty list, not an error", "[mention_resolver]") {
    TempDir dir;
    CHECK(Resolver().Suggest("nope/", dir.path(), 50).empty());
}

TEST_CASE("results are sorted with directories and files interleaved", "[mention_resolver]") {
    TempDir dir;
    MakeDir(dir, "zeta");
    WriteFile(dir, "alpha.txt");
    WriteFile(dir, "mango.txt");

    const auto paths = Resolver().Suggest("", dir.path(), 50);

    CHECK(paths == std::vector<std::string>{"alpha.txt", "mango.txt", "zeta/"});
}

TEST_CASE("limit truncates a deterministic head", "[mention_resolver]") {
    TempDir dir;
    for (const auto* name : {"a", "b", "c", "d", "e"}) WriteFile(dir, std::string(name) + ".txt");

    const auto paths = Resolver().Suggest("", dir.path(), 3);

    CHECK(paths == std::vector<std::string>{"a.txt", "b.txt", "c.txt"});
}

TEST_CASE("limit <= 0 takes the default", "[mention_resolver]") {
    TempDir dir;
    WriteFile(dir, "only.txt");

    CHECK(Resolver().Suggest("", dir.path(), 0) == std::vector<std::string>{"only.txt"});
    CHECK(Resolver().Suggest("", dir.path(), -7) == std::vector<std::string>{"only.txt"});
}

TEST_CASE("the registry is exclusive: first registration wins", "[mention_resolver]") {
    agent::MentionResolverRegistry registry(common::DefaultLogger());

    class Fixed : public agent::MentionResolver {
    public:
        explicit Fixed(std::string marker) : marker_(std::move(marker)) {}
        std::vector<std::string> Suggest(std::string_view, const std::filesystem::path&,
                                         int) const override {
            return {marker_};
        }
    private:
        std::string marker_;
    };

    CHECK_FALSE(registry.has_override());
    CHECK(registry.TrySetOverride(std::make_shared<Fixed>("first")));
    CHECK_FALSE(registry.TrySetOverride(std::make_shared<Fixed>("second")));

    // The second call is a warning and a no-op, signaled by its false return --
    // which strategy is in force must never depend on load order.
    REQUIRE(registry.has_override());
    TempDir dir;
    CHECK(registry.Active().Suggest("", dir.path(), 50) == std::vector<std::string>{"first"});
}

TEST_CASE("the registry falls back to the core default", "[mention_resolver]") {
    agent::MentionResolverRegistry registry(common::DefaultLogger());
    CHECK_FALSE(registry.has_override());

    TempDir dir;
    WriteFile(dir, "main.cpp");
    CHECK(registry.Active().Suggest("ma", dir.path(), 50) == std::vector<std::string>{"main.cpp"});
}
