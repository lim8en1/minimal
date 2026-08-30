#include "minimal/plugins/collision_check.hpp"

#include <sstream>

namespace minimal::plugins {

CollisionReport CheckRegistrationNames(const tools::ToolRegistry& tools,
                                       const commands::CommandRegistry& commands,
                                       const std::map<std::string, std::string>& command_aliases) {
    NameRegistry names;

    // Built-ins registered by core itself carry an empty plugin name. They still
    // participate: a plugin shadowing `skill` is exactly as load-order-dependent
    // as one plugin shadowing another.
    for (const auto& attribution : tools.attributions()) {
        names.Register(NameKind::Tool, attribution.plugin_name, attribution.tool_name);
    }
    for (const auto& attribution : commands.attributions()) {
        names.Register(NameKind::Command, attribution.plugin_name, attribution.original_name);
    }
    // Aliases are applied BEFORE resolution, which is what makes aliasing a valid
    // way to RESOLVE a collision rather than merely a way to avoid causing one.
    for (const auto& [qualified, alias] : command_aliases) {
        names.SetAlias(NameKind::Command, qualified, alias);
    }

    const auto resolved = names.Resolve();

    CollisionReport report;
    report.dangling_aliases = resolved.dangling_aliases;

    // NameRegistry says WHICH name collided; the entries say who claimed it.
    // Joined here rather than inside NameRegistry so that registry keeps the
    // narrow, already-tested shape it has.
    for (const auto& name : resolved.fatal_collisions) {
        Collision collision;
        collision.surface_name = name;
        for (const auto& entry : resolved.entries) {
            if (entry.surface_name != name) continue;
            collision.claimed_by.push_back(entry.stable_identity);
        }
        // Tools and commands are separate namespaces, so a name can collide in
        // one and be fine in the other; say which.
        collision.kind = "registration";
        for (const auto& attribution : commands.attributions()) {
            if (attribution.surface_name == name) collision.kind = "command";
        }
        for (const auto& attribution : tools.attributions()) {
            if (attribution.tool_name == name) collision.kind = "tool";
        }
        report.fatal.push_back(std::move(collision));
    }
    return report;
}

std::string DescribeCollisions(const CollisionReport& report) {
    std::ostringstream out;
    for (const auto& collision : report.fatal) {
        out << "  " << collision.kind << " name '" << collision.surface_name
            << "' is claimed by:";
        for (const auto& claimant : collision.claimed_by) {
            // A built-in registered by core carries no plugin name.
            out << " " << (claimant.front() == '.' ? "(core)" + claimant : claimant);
        }
        out << "\n";
    }
    for (const auto& dangling : report.dangling_aliases) {
        out << "  alias names nothing registered: " << dangling << "\n";
    }
    return out.str();
}

}  // namespace minimal::plugins
