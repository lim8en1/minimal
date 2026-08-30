// minimal-server entry point.
//
// Startup ordering is load-bearing and must not be rearranged casually:
//   config load -> plugin load -> sanity check -> serve
// Providers and tools are plugin-registered, so "is there at least one usable
// provider?" cannot be answered before plugins are in. See ARCHITECTURE.md.
//
// Only the first two stages exist so far; the sanity check and the gRPC service
// slot in after them.

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/config/skill_registry.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/plugins/plugin_discovery.hpp"
#include "minimal/plugins/plugin_loader.hpp"
#include "minimal/tools/skill_tool.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace {

std::string ReadFileOrEmpty(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void WriteFile(const std::filesystem::path& path, const std::string& contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (out) out << contents;
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    const auto config_dir = minimal::common::ConfigDir();

    std::printf("minimal-server %s\n", minimal::common::Version());
    std::printf("  abi tag:    %s\n", minimal::common::AbiTag());
    std::printf("  config dir: %s\n", config_dir.c_str());
    std::printf("  data dir:   %s\n", minimal::common::DataDir().c_str());

    // --- Stage 1: config -----------------------------------------------------
    const auto plugins_json = config_dir / "plugins.json";
    auto plugin_config = minimal::plugins::ParsePluginConfig(ReadFileOrEmpty(plugins_json));

    // Frontmatter-only scan; bodies stay on disk until a skill is invoked.
    const auto skills = minimal::config::SkillRegistry::Load(config_dir);

    // --- Stage 2: plugins ----------------------------------------------------
    const auto bundled_dir = minimal::plugins::BundledPluginDir();
    const auto user_dir = config_dir / "plugins";
    std::printf("  plugin dir: %s\n\n", bundled_dir.c_str());

    auto candidates =
        minimal::plugins::DiscoverPlugins(bundled_dir, user_dir, plugin_config);

    // Persist anything newly discovered, so its enablement is user-editable.
    WriteFile(plugins_json, minimal::plugins::SerializePluginConfig(plugin_config));

    minimal::tools::ToolRegistry tools;
    minimal::llm::ProviderFactoryRegistry providers;
    minimal::plugins::PluginLoader loader(tools, providers);
    const auto records = loader.LoadAll(candidates);

    std::printf("plugins (%zu discovered):\n", records.size());
    for (const auto& record : records) {
        std::printf("  %-18s %-12s %s\n",
                    record.name.empty() ? record.file.filename().c_str() : record.name.c_str(),
                    std::string(minimal::plugins::ToString(record.status)).c_str(),
                    record.error.c_str());
        for (const auto& tool : record.registered_tools) {
            std::printf("      tool     %s\n", tool.c_str());
        }
        for (const auto& kind : record.registered_provider_kinds) {
            std::printf("      provider %s\n", kind.c_str());
        }
    }

    // Core tools are registered by the harness itself, not by a plugin: the
    // empty attribution marks them as built-ins for collision reporting.
    tools.Register(std::make_unique<minimal::tools::SkillTool>(skills));

    std::printf("skills (%zu):\n", skills.All().size());
    for (const auto& skill : skills.All()) {
        if (skill.loaded) {
            std::printf("  %-16s %s\n", skill.name.c_str(), skill.description.c_str());
        } else {
            std::printf("  %-16s NOT LOADED: %s\n", skill.name.c_str(), skill.load_error.c_str());
        }
    }

    std::printf("\nregistered: %zu tool(s), %zu provider kind(s)\n", tools.size(),
                providers.kinds().size());

    // TODO: stage 3 sanity check (bundled plugin failures are Fatal, user
    // failures Warning), then stage 4 serve SessionService.
    return 0;
}
