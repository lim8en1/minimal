#include "minimal/tools/aliased_tool.hpp"

#include <utility>

namespace minimal::tools {

AliasedTool::AliasedTool(std::shared_ptr<Tool> inner, std::string surface_name)
    : inner_(std::move(inner)), spec_(inner_->spec()) {
    spec_.name = std::move(surface_name);
    spec_.definition.name = spec_.name;
}

const ToolSpec& AliasedTool::spec() const { return spec_; }

ToolResult AliasedTool::execute(const nlohmann::json& input, ToolContext& ctx) {
    return inner_->execute(input, ctx);
}

bool AliasedTool::path_confinement_supported() const { return inner_->path_confinement_supported(); }

std::optional<std::string> AliasedTool::ExtractPath(const nlohmann::json& input) const {
    return inner_->ExtractPath(input);
}

// The declarative virtuals must forward like everything above: through the
// wrapper they would otherwise fall back to the Tool defaults (network false,
// parallel_safe true, no narrowing) and an aliased web_fetch would skip the
// network permission fold while an aliased bash lost its parallel dispatch
// opt-out.
bool AliasedTool::network_access_required() const { return inner_->network_access_required(); }

// Pattern rules key on the PERMISSION key, but the subject comes from the
// tool itself -- which is the wrapped one. Without this forward an aliased
// bash would present no subject and every bash pattern would silently stop
// matching.
std::optional<std::string> AliasedTool::PatternSubject(const nlohmann::json& input) const {
    return inner_->PatternSubject(input);
}

std::string_view AliasedTool::config_namespace() const { return inner_->config_namespace(); }

bool AliasedTool::parallel_safe() const { return inner_->parallel_safe(); }

std::optional<PermissionMode> AliasedTool::mode_override_for(const nlohmann::json& input,
                                                             const ToolContext& ctx) const {
    return inner_->mode_override_for(input, ctx);
}

std::optional<ToolCategory> AliasedTool::category_for(const nlohmann::json& input) const {
    return inner_->category_for(input);
}

PermissionDisclosure AliasedTool::Disclose(const nlohmann::json& input, const ToolContext& ctx) const {
    PermissionDisclosure disclosure = inner_->Disclose(input, ctx);
    // A tool that does not override Disclose inherits the base default: its own
    // name, no grants, complete. Through the wrapper that default would advertise
    // the ORIGINAL name in the Ask prompt while the model called it by its alias.
    // Recognise exactly that shape and restate it under the surface name; any
    // real override (custom summary or grants) passes through untouched.
    if (disclosure.grants.empty() && disclosure.complete && disclosure.summary == inner_->spec().name) {
        disclosure.summary = spec_.name;
    }
    return disclosure;
}

}  // namespace minimal::tools
