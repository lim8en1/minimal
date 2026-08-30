#include "minimal/agent/subagent_spawn.hpp"

#include <algorithm>
#include <unordered_set>

namespace minimal::agent {

std::vector<std::string> intersect_allowed(const std::vector<std::string>& parent,
                                           const std::vector<std::string>& child) {
    const auto has_star = [](const std::vector<std::string>& list) {
        return std::find(list.begin(), list.end(), "*") != list.end();
    };
    if (has_star(parent)) return child;
    if (has_star(child)) return parent;

    const std::unordered_set<std::string> in_parent(parent.begin(), parent.end());
    std::unordered_set<std::string> seen;
    std::vector<std::string> out;
    for (const auto& name : child) {
        if (in_parent.count(name) != 0 && seen.insert(name).second) out.push_back(name);
    }
    return out;
}

}  // namespace minimal::agent
