#pragma once

#include <memory>
#include <string>

#include "minimal/tools/tool.hpp"

namespace minimal::tools {

// A tool registered under a surface name different from the one it declared --
// the plugins.json alias mechanism (ARCHITECTURE.md, "Name collisions and
// aliases"). The ONLY difference from the wrapped tool is spec(): `name` and
// `definition.name` are rewritten to the alias, so what the model sees matches
// the registry key. Everything else -- execute, disclosure, path extraction,
// category -- behaves exactly as the plugin wrote it.
//
// The wrapper exists because ToolSpec is const on every Tool: without it an
// alias could re-key the registry map while the definition still advertised the
// original name, and a model calling the advertised name would find nothing.
class AliasedTool : public Tool {
public:
    AliasedTool(std::unique_ptr<Tool> inner, std::string surface_name);

    const ToolSpec& spec() const override;
    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override;
    bool path_confinement_supported() const override;
    std::optional<std::string> ExtractPath(const nlohmann::json& input) const override;
    std::optional<ToolCategory> category_for(const nlohmann::json& input) const override;
    PermissionDisclosure Disclose(const nlohmann::json& input, const ToolContext& ctx) const override;

private:
    std::unique_ptr<Tool> inner_;
    ToolSpec spec_;  // a copy of inner_->spec() with name/definition.name rewritten
};

}  // namespace minimal::tools
