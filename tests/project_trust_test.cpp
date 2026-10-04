#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "minimal/config/project_trust.hpp"
#include "minimal/config/settings_config.hpp"

namespace fs = std::filesystem;
using minimal::config::ProjectLocalMode;
using minimal::config::ProjectTrust;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("project_trust_" + std::to_string(::getpid()) + "_" + std::to_string(counter()++));
        fs::create_directories(path);
        path = fs::canonical(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    static int& counter() { static int n = 0; return n; }
};

// A git-looking project: <root>/.git plus a nested working directory.
fs::path MakeRepo(const fs::path& base, const std::string& name) {
    const fs::path root = base / name;
    fs::create_directories(root / ".git");
    fs::create_directories(root / "src" / "deep");
    return root;
}

}  // namespace

TEST_CASE("trust covers the whole repository, not just the directory named", "[project_trust]") {
    TempDir tmp;
    const fs::path repo = MakeRepo(tmp.path, "repo");
    ProjectTrust trust(tmp.path / "state" / "trusted.json", ProjectLocalMode::kTrusted);

    CHECK_FALSE(trust.Allows(repo));
    std::string error;
    REQUIRE(trust.Trust(repo / "src" / "deep", error));  // granted from a subdirectory
    CHECK(trust.Allows(repo));
    CHECK(trust.Allows(repo / "src"));
    CHECK_FALSE(trust.Allows(MakeRepo(tmp.path, "other")));
    CHECK(ProjectTrust::RootFor(repo / "src") == repo);
}

TEST_CASE("a directory without a git root is its own trust root", "[project_trust]") {
    TempDir tmp;
    const fs::path loose = tmp.path / "loose" / "sub";
    fs::create_directories(loose);
    ProjectTrust trust(tmp.path / "trusted.json", ProjectLocalMode::kTrusted);
    std::string error;
    REQUIRE(trust.Trust(loose, error));
    CHECK(trust.Allows(loose));
    CHECK_FALSE(trust.Allows(loose.parent_path()));
}

TEST_CASE("modes: all allows everything, none allows nothing, an empty root nothing",
          "[project_trust]") {
    TempDir tmp;
    const fs::path repo = MakeRepo(tmp.path, "repo");
    CHECK(ProjectTrust(tmp.path / "a.json", ProjectLocalMode::kAll).Allows(repo));
    ProjectTrust none(tmp.path / "b.json", ProjectLocalMode::kNone);
    std::string error;
    REQUIRE(none.Trust(repo, error));
    CHECK_FALSE(none.Allows(repo));  // an explicit grant does not override "none"
    CHECK_FALSE(ProjectTrust(tmp.path / "c.json", ProjectLocalMode::kAll).Allows({}));
}

TEST_CASE("grants persist across instances and revoke removes them", "[project_trust]") {
    TempDir tmp;
    const fs::path repo = MakeRepo(tmp.path, "repo");
    const fs::path state = tmp.path / "trusted.json";
    {
        ProjectTrust trust(state, ProjectLocalMode::kTrusted);
        std::string error;
        REQUIRE(trust.Trust(repo, error));
    }
    ProjectTrust reloaded(state, ProjectLocalMode::kTrusted);
    CHECK(reloaded.Allows(repo));
    const auto before = reloaded.generation();
    std::string error;
    REQUIRE(reloaded.Revoke(repo, error));
    CHECK_FALSE(reloaded.Allows(repo));
    CHECK(reloaded.generation() > before);
    CHECK_FALSE(ProjectTrust(state, ProjectLocalMode::kTrusted).Allows(repo));
}

TEST_CASE("generation moves only when the trusted set changes", "[project_trust]") {
    TempDir tmp;
    const fs::path repo = MakeRepo(tmp.path, "repo");
    ProjectTrust trust(tmp.path / "t.json", ProjectLocalMode::kTrusted);
    std::string error;
    const auto start = trust.generation();
    REQUIRE(trust.Trust(repo, error));
    const auto granted = trust.generation();
    CHECK(granted > start);
    REQUIRE(trust.Trust(repo, error));   // already trusted
    REQUIRE(trust.Revoke(MakeRepo(tmp.path, "never"), error));  // never trusted
    CHECK(trust.generation() == granted);
}

TEST_CASE("a corrupt state file trusts nothing", "[project_trust]") {
    TempDir tmp;
    const fs::path repo = MakeRepo(tmp.path, "repo");
    const fs::path state = tmp.path / "trusted.json";
    {
        std::ofstream out(state);
        out << "{ not json";
    }
    ProjectTrust trust(state, ProjectLocalMode::kTrusted);
    CHECK_FALSE(trust.Allows(repo));
    std::string error;
    REQUIRE(trust.Trust(repo, error));  // and is recoverable by trusting again
    CHECK(ProjectTrust(state, ProjectLocalMode::kTrusted).Allows(repo));
}

TEST_CASE("a failed write leaves the in-memory set unchanged", "[project_trust]") {
    TempDir tmp;
    const fs::path repo = MakeRepo(tmp.path, "repo");
    // The state "file" is a directory: the write cannot succeed.
    const fs::path state = tmp.path / "trusted.json";
    fs::create_directories(state);
    ProjectTrust trust(state, ProjectLocalMode::kTrusted);
    std::string error;
    CHECK_FALSE(trust.Trust(repo, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(trust.Allows(repo));
}

TEST_CASE("concurrent grants all land", "[project_trust]") {
    TempDir tmp;
    ProjectTrust trust(tmp.path / "t.json", ProjectLocalMode::kTrusted);
    std::vector<fs::path> repos;
    for (int i = 0; i < 8; ++i) repos.push_back(MakeRepo(tmp.path, "r" + std::to_string(i)));
    std::vector<std::thread> threads;
    for (const auto& repo : repos) {
        threads.emplace_back([&trust, repo] {
            std::string error;
            trust.Trust(repo, error);
        });
    }
    for (auto& thread : threads) thread.join();
    for (const auto& repo : repos) CHECK(trust.Allows(repo));
    CHECK(trust.trusted().size() == 8);
}

TEST_CASE("project_local.mode parses and rejects unknown values", "[project_trust][settings]") {
    using minimal::config::ParseSettingsConfig;
    CHECK(ParseSettingsConfig(nlohmann::json::object()).project_local.mode ==
          ProjectLocalMode::kTrusted);
    CHECK(ParseSettingsConfig({{"project_local", {{"mode", "all"}}}}).project_local.mode ==
          ProjectLocalMode::kAll);
    CHECK(ParseSettingsConfig({{"project_local", {{"mode", "none"}}}}).project_local.mode ==
          ProjectLocalMode::kNone);
    CHECK_THROWS(ParseSettingsConfig({{"project_local", {{"mode", "sometimes"}}}}));
    CHECK_THROWS(ParseSettingsConfig({{"project_local", "all"}}));
}
