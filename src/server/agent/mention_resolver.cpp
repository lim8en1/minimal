#include "minimal/agent/mention_resolver.hpp"
#include "minimal/agent/policy.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace minimal::agent {

std::vector<std::string> PrefixMentionResolver::Suggest(std::string_view prefix,
                                                        const std::filesystem::path& project_root,
                                                        int limit) const {
    if (limit <= 0) limit = kDefaultMentionSuggestLimit;

    // The deepest complete component is everything up to and including the last
    // '/'. Descent ONLY on '/': that rule is what keeps a keystroke O(entries of
    // one directory) instead of a walk.
    const auto slash = prefix.rfind('/');
    const std::string_view dir_part =
        slash == std::string_view::npos ? std::string_view{} : prefix.substr(0, slash + 1);
    const std::string_view fragment =
        slash == std::string_view::npos ? prefix : prefix.substr(slash + 1);

    // dir_part is raw client input. Absolute ("@/etc/pa") would REPLACE the
    // root in operator/ and ".." would climb out of it, turning the suggest
    // endpoint into a host-wide directory-enumeration oracle. Reject both
    // before building `dir`.
    const std::filesystem::path requested(dir_part);
    if (requested.is_absolute()) return {};
    for (const auto& part : requested) {
        if (part == "..") return {};
    }

    const std::filesystem::path dir =
        dir_part.empty() ? project_root : project_root / std::filesystem::path(dir_part);

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return {};

    // Second gate, for the entries the lexical check above cannot see: a
    // symlink INSIDE the root may point out of it (following such links is
    // today's intended semantics for in-root dirs, but only within the root).
    // Containment is therefore decided on the CANONICAL path -- what will
    // actually be iterated -- and anything that escapes project_root is
    // refused. lexically_relative() is empty when the roots diverge and leads
    // with ".." on escape, so both spellings of escape reject here.
    std::error_code root_ec;
    const std::filesystem::path canonical_dir = std::filesystem::weakly_canonical(dir, ec);
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(project_root, root_ec);
    if (ec || root_ec) return {};
    const std::filesystem::path rel = canonical_dir.lexically_relative(canonical_root);
    if (rel.empty() || *rel.begin() == "..") return {};

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
    override_owner_ = attribution_;
    return true;
}

void MentionResolverRegistry::SetAttribution(std::string plugin_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    attribution_ = std::move(plugin_name);
}

void MentionResolverRegistry::ClearAttribution() {
    std::lock_guard<std::mutex> lock(mutex_);
    attribution_.clear();
}

bool MentionResolverRegistry::OwnedBy(const std::string& plugin_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return override_ != nullptr && override_owner_ == plugin_name;
}

void MentionResolverRegistry::RemoveByAttribution(const std::string& plugin_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (override_ != nullptr && override_owner_ == plugin_name) {
        override_.reset();
        override_owner_.clear();
    }
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
