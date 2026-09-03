#include "minimal/agent/mention_resolver.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace minimal::agent {

namespace {

constexpr int kDefaultSuggestLimit = 50;

}  // namespace

std::vector<std::string> PrefixMentionResolver::Suggest(std::string_view prefix,
                                                        const std::filesystem::path& project_root,
                                                        int limit) const {
    if (limit <= 0) limit = kDefaultSuggestLimit;

    // The deepest complete component is everything up to and including the last
    // '/'. Descent ONLY on '/': that rule is what keeps a keystroke O(entries of
    // one directory) instead of a walk.
    const auto slash = prefix.rfind('/');
    const std::string_view dir_part =
        slash == std::string_view::npos ? std::string_view{} : prefix.substr(0, slash + 1);
    const std::string_view fragment =
        slash == std::string_view::npos ? prefix : prefix.substr(slash + 1);

    const std::filesystem::path dir =
        dir_part.empty() ? project_root : project_root / std::filesystem::path(dir_part);

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return {};

    // directory_iterator order is unspecified -- collect, sort, then truncate,
    // so the result is deterministic and the limit cuts the head of a stable
    // ordering rather than a random one.
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (!name.starts_with(fragment)) continue;
        names.push_back(name);
    }
    if (ec) return {};

    // Dotfiles are invisible unless asked for, like bash completion: a user
    // typing "sr" does not want .gitignore crowding the list.
    const bool fragment_wants_dotfiles = !fragment.empty() && fragment.front() == '.';
    std::erase_if(names, [&](const std::string& name) {
        return !fragment_wants_dotfiles && !name.empty() && name.front() == '.';
    });

    std::sort(names.begin(), names.end());

    std::vector<std::string> out;
    out.reserve(std::min<std::size_t>(names.size(), static_cast<std::size_t>(limit)));
    for (const auto& name : names) {
        if (static_cast<int>(out.size()) >= limit) break;
        std::error_code type_ec;
        const bool is_dir = std::filesystem::is_directory(dir / name, type_ec);
        std::string relative;
        relative.reserve(dir_part.size() + name.size() + 1);
        relative.append(dir_part);
        relative.append(name);
        if (is_dir) relative.push_back('/');
        out.push_back(std::move(relative));
    }
    return out;
}

MentionResolverRegistry::MentionResolverRegistry(common::Logger& logger) : logger_(logger) {}

bool MentionResolverRegistry::TrySetOverride(std::shared_ptr<MentionResolver> resolver) {
    if (resolver == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (override_ != nullptr) {
        logger_.warn("a mention resolver is already registered; ignoring a second one -- "
                     "the seam is exclusive, first registration wins");
        return false;
    }
    override_ = std::move(resolver);
    return true;
}

const MentionResolver& MentionResolverRegistry::Active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (override_ != nullptr) return *override_;
    return default_;
}

bool MentionResolverRegistry::has_override() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return override_ != nullptr;
}

}  // namespace minimal::agent
