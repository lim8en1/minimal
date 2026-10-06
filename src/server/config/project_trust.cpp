#include "minimal/config/project_trust.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

#include "minimal/common/logger.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/config/project_dirs.hpp"
#include "minimal/store/file_lock.hpp"
#include "minimal/common/atomic_write.hpp"

namespace minimal::config {

namespace {

// The recorded grants. Missing is "nothing trusted"; so is an unreadable or
// malformed file -- the fail-closed direction -- and the problem is logged.
std::set<std::string> ReadRoots(const std::filesystem::path& state_file) {
    std::set<std::string> roots;
    std::error_code ec;
    if (!std::filesystem::exists(state_file, ec) || ec) return roots;
    try {
        const auto file = common::ReadFile(state_file, 1u << 20);
        if (!file.ok()) throw std::runtime_error(file.error);
        const auto doc = nlohmann::json::parse(file.contents);
        for (const auto& entry : doc.at("trusted")) roots.insert(entry.get<std::string>());
    } catch (const std::exception& error) {
        roots.clear();
        common::DefaultLogger().warn("{}: ignoring unreadable trusted-directories file: {}",
                                     state_file.string(), error.what());
    }
    return roots;
}

}  // namespace

ProjectTrust::ProjectTrust(std::filesystem::path state_file, ProjectLocalMode mode)
    : state_file_(std::move(state_file)), mode_(mode), roots_(ReadRoots(state_file_)) {}

std::filesystem::path ProjectTrust::RootFor(const std::filesystem::path& project_root) {
    const ProjectDirs dirs(project_root);
    return dirs.dirs().empty() ? std::filesystem::path{} : dirs.dirs().back();
}

namespace {

std::size_t CountFiles(const std::filesystem::path& dir, std::string_view extension) {
    std::error_code ec;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code type_ec;
        if (it->is_regular_file(type_ec) && it->path().extension() == extension &&
            it->path().filename().string().front() != '.') {
            ++count;
        }
    }
    return count;
}

std::string Plural(std::size_t n, const char* singular, const char* plural) {
    return std::to_string(n) + " " + (n == 1 ? singular : plural);
}

}  // namespace

ProjectLocalInventory InspectProjectLocal(const std::filesystem::path& project_root,
                                          const ProjectContentProvider* plugins) {
    std::vector<ProjectContentCount> found{{"agent", "agents", 0}};
    const ProjectDirs dirs(project_root);
    for (const auto& dir : dirs.dirs()) found[0].count += CountFiles(dir / "agents", ".md");
    if (plugins != nullptr) plugins->DescribeProjectContent(project_root, found);

    ProjectLocalInventory out;
    for (const auto& item : found) {
        if (item.count == 0) continue;
        const auto same = std::find_if(out.items.begin(), out.items.end(),
                                       [&](const auto& e) { return e.singular == item.singular; });
        if (same != out.items.end()) {
            same->count += item.count;
        } else {
            out.items.push_back(item);
        }
    }
    return out;
}

std::size_t ProjectLocalInventory::CountOf(std::string_view singular) const {
    for (const auto& item : items) {
        if (item.singular == singular) return item.count;
    }
    return 0;
}

std::string ProjectLocalInventory::Describe() const {
    std::vector<std::string> parts;
    for (const auto& item : items) {
        parts.push_back(Plural(item.count, item.singular.c_str(), item.plural.c_str()));
    }
    if (parts.empty()) return "nothing";
    std::string joined = parts.front();
    for (std::size_t i = 1; i < parts.size(); ++i) joined += ", " + parts[i];
    return joined;
}

bool ProjectTrust::IsTrusted(const std::filesystem::path& project_root) const {
    const std::filesystem::path root = RootFor(project_root);
    if (root.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return roots_.count(root.string()) != 0;
}

bool ProjectTrust::Allows(const std::filesystem::path& project_root) const {
    if (project_root.empty()) return false;
    switch (mode_) {
        case ProjectLocalMode::kAll: return true;
        case ProjectLocalMode::kNone: return false;
        case ProjectLocalMode::kTrusted: return IsTrusted(project_root);
    }
    return false;
}

bool ProjectTrust::Update(const std::filesystem::path& project_root, bool trust,
                          std::string& error) {
    const std::filesystem::path root = RootFor(project_root);
    if (root.empty()) {
        if (!trust) return true;
        error = "no project directory to trust";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // Read-modify-write under the cross-process lock, starting from the FILE:
    // another server sharing this config dir may have recorded grants since
    // this one loaded, and writing the in-memory set would drop them.
    std::optional<store::ExclusiveFileLock> file_lock;
    try {
        std::filesystem::create_directories(state_file_.parent_path());
        file_lock.emplace(state_file_);
    } catch (const std::exception& e) {
        error = "could not lock " + state_file_.string() + ": " + e.what();
        return false;
    }
    const std::set<std::string> recorded = ReadRoots(state_file_);
    std::set<std::string> next = recorded;
    if (trust) {
        next.insert(root.string());
    } else {
        next.erase(root.string());
    }
    if (next != recorded) {
        nlohmann::json doc{{"version", 1}, {"trusted", nlohmann::json::array()}};
        for (const auto& entry : next) doc["trusted"].push_back(entry);
        const auto outcome = common::write_file(
            state_file_, doc.dump(2) + "\n",
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        if (!outcome.ok) {
            error = outcome.error;
            return false;
        }
    }
    roots_ = std::move(next);
    return true;
}

bool ProjectTrust::Trust(const std::filesystem::path& project_root, std::string& error) {
    return Update(project_root, /*trust=*/true, error);
}

bool ProjectTrust::Revoke(const std::filesystem::path& project_root, std::string& error) {
    return Update(project_root, /*trust=*/false, error);
}

std::vector<std::filesystem::path> ProjectTrust::trusted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {roots_.begin(), roots_.end()};
}

}  // namespace minimal::config
