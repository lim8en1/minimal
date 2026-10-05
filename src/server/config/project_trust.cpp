#include "minimal/config/project_trust.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

#include "minimal/common/logger.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/config/project_dirs.hpp"
#include "minimal/tools/atomic_write.hpp"

namespace minimal::config {

ProjectTrust::ProjectTrust(std::filesystem::path state_file, ProjectLocalMode mode)
    : state_file_(std::move(state_file)), mode_(mode) {
    std::error_code ec;
    if (!std::filesystem::exists(state_file_, ec) || ec) return;  // nothing trusted yet
    try {
        const auto file = common::ReadFile(state_file_, 1u << 20);
        if (!file.ok()) throw std::runtime_error(file.error);
        const auto doc = nlohmann::json::parse(file.contents);
        for (const auto& entry : doc.at("trusted")) roots_.insert(entry.get<std::string>());
    } catch (const std::exception& error) {
        // Fail closed: an unreadable record trusts nothing.
        roots_.clear();
        common::DefaultLogger().warn("{}: ignoring unreadable trusted-directories file: {}",
                                     state_file_.string(), error.what());
    }
}

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

bool ProjectTrust::Persist(const std::set<std::string>& roots, std::string& error) const {
    nlohmann::json doc{{"version", 1}, {"trusted", nlohmann::json::array()}};
    for (const auto& root : roots) doc["trusted"].push_back(root);
    std::error_code ec;
    std::filesystem::create_directories(state_file_.parent_path(), ec);
    const auto outcome = tools::write_file(
        state_file_, doc.dump(2) + "\n",
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    if (!outcome.ok) error = outcome.error;
    return outcome.ok;
}

bool ProjectTrust::Trust(const std::filesystem::path& project_root, std::string& error) {
    const std::filesystem::path root = RootFor(project_root);
    if (root.empty()) {
        error = "no project directory to trust";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::string> next = roots_;
    next.insert(root.string());
    if (next == roots_) return true;
    if (!Persist(next, error)) return false;
    roots_ = std::move(next);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool ProjectTrust::Revoke(const std::filesystem::path& project_root, std::string& error) {
    const std::filesystem::path root = RootFor(project_root);
    if (root.empty()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (roots_.count(root.string()) == 0) return true;
    std::set<std::string> next = roots_;
    next.erase(root.string());
    if (!Persist(next, error)) return false;
    roots_ = std::move(next);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

std::vector<std::filesystem::path> ProjectTrust::trusted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {roots_.begin(), roots_.end()};
}

}  // namespace minimal::config
