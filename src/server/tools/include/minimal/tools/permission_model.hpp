#pragma once

// Plugin-owned permission models: core owns the permission MACHINERY, a plugin
// owns the permission MODEL for the tools it registered.
//
// The core model (categories, per-tool rules, patterns, the out_of_workdir and
// network dimensions -- PermissionPolicy) governs every tool no model claims.
// A plugin whose tools need a finer or differently-shaped model (per-host rules
// for a web fetcher, per-server modes for MCP tools) registers a
// PermissionModel that CLAIMS its own tools and REPLACES one or more core
// resolution stages for them. Everything around those stages stays core and
// cannot be bypassed by a model:
//
//   - an operator per-tool DENY (permissions.json / preset per_tool_overrides)
//     is a kill switch folded over the model's answer;
//   - the subagent ceiling: core resolves the model a second time against the
//     PARENT's layer and takes the most_restrictive meet -- models never see
//     ceilings;
//   - out_of_workdir (path confinement is core);
//   - the tool's own mode_override_for narrowing, the run mode, session grants
//     and the grant filter, hooks, the advisor, and the human prompt;
//   - fail-closed: a model that throws resolves to Deny, naming the model.
//
// Why this does not widen trust: a plugin already runs arbitrary code
// in-process and its tools' execute() can do anything. Ruling on its OWN tools
// adds no power it did not have. What a model may NOT do is rule on another
// plugin's tools -- the startup sanity check makes a foreign claim fatal
// (PermissionModelRegistry::ValidateClaims).
//
// Configuration: a model's slice is opaque to core. permissions.json carries
// the operator's global slice under `plugins.<model>`, a preset's frontmatter
// carries its slice under `plugin_permissions.<model>`; BuildLayer turns the
// pair into one immutable per-preset layer. `preset_trusted` is false for a
// project-local (repo-shipped) preset, which may only narrow -- the model
// enforces that for its own slice, exactly as BuildPermissionPolicy does for
// the core model.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/plugins/attributed_registry.hpp"
#include "minimal/tools/permission.hpp"

namespace minimal::tools {

class Tool;
struct ToolContext;
class ToolRegistry;

// One model's per-preset configuration, built by BuildLayer and read back only
// by the same model (which knows its concrete type). Immutable and shared: a
// preset's layer and every session's copy of it are one object.
using PermissionLayer = std::shared_ptr<const void>;

// The core resolution stages a model may take over for the tools it claims.
// Bit flags: a model may replace several.
enum class PermissionStage : unsigned {
    // The tool/category resolution (PermissionPolicy::mode_for): the base mode
    // before any dimension folds. The stage the MCP model replaces.
    kBase = 1u << 0,
    // The `network` dimension fold (a tool that declares
    // network_access_required()). A per-host model replaces it.
    kNetwork = 1u << 1,
};

constexpr unsigned operator|(PermissionStage a, PermissionStage b) {
    return static_cast<unsigned>(a) | static_cast<unsigned>(b);
}
constexpr bool replaces(unsigned mask, PermissionStage stage) {
    return (mask & static_cast<unsigned>(stage)) != 0;
}

// Everything one Resolve call may consult. Borrowed for the call only.
struct PermissionQuery {
    const Tool& tool;
    // The PERMISSION key (the non-alias name) -- what claims and per-tool
    // rules key on, so an alias never changes which model governs a tool.
    std::string_view permission_key;
    const nlohmann::json& input;
    const ToolContext& ctx;
    // The core policy at the SAME scope as `layer`: this session's, or the
    // parent's when core resolves the ceiling. A model may consult its
    // generic per-tool rules on permission_key (the MCP model refines a
    // server's mode by a rule on the full mcp__ name). Null when that scope
    // carries no core policy.
    const PermissionPolicy* core_policy = nullptr;
    // This model's layer at the same scope. Null when the scope has none (a
    // preset built before the model was registered, a test).
    const void* layer = nullptr;
};

struct PermissionAnswer {
    PermissionMode mode = PermissionMode::Ask;
    // The rule that decided, in the words a denial and a prompt show the
    // operator ("mcp default gh=ask", "host `*.internal`=deny").
    std::string why;
};

class PermissionModel {
public:
    virtual ~PermissionModel() = default;

    // PermissionStage bits this model resolves for the tools it claims.
    // Fixed for the model's lifetime.
    virtual unsigned Replaces() const = 0;

    // True for tools this model governs. Must answer only for tools the
    // registering plugin owns (ValidateClaims enforces it at startup). Cheap
    // and pure: it runs on every gated call.
    virtual bool Claims(std::string_view permission_key) const = 0;

    // Builds a preset's layer from the operator's global slice
    // (permissions.json `plugins.<model>`) and the preset's slice
    // (`plugin_permissions.<model>`); either may be null (absent). Throws on
    // a malformed slice -- surfaced as that preset's load error.
    virtual PermissionLayer BuildLayer(const nlohmann::json& global_slice,
                                       const nlohmann::json& preset_slice,
                                       bool preset_trusted) const = 0;

    // Resolves one replaced `stage` for one claimed invocation. Pure; runs
    // on the session worker per gated call. May throw -- core answers Deny.
    virtual PermissionAnswer Resolve(PermissionStage stage, const PermissionQuery& query) const = 0;

    // One line for `/permissions show`. Default: nothing.
    virtual std::string Describe(const void* layer) const {
        (void)layer;
        return {};
    }
};

// A model together with one scope's layer: what an executor holds.
struct BoundPermissionModel {
    std::string name;  // the model's registry key (its plugin, or a core built-in)
    std::shared_ptr<const PermissionModel> model;
    PermissionLayer layer;
};
using PermissionModels = std::vector<BoundPermissionModel>;

// The first bound model claiming `permission_key`, or null. ValidateClaims
// guarantees at most one exists for a statically registered tool.
const BoundPermissionModel* FindClaimant(const PermissionModels& models,
                                         std::string_view permission_key);

// Keyed by the registering plugin's name -- one model per plugin.
class PermissionModelRegistry
    : public plugins::AttributedRegistry<std::shared_ptr<const PermissionModel>> {
public:
    // A plugin's model: keyed by the plugin's own name (the loader's
    // attribution). A second model from the same plugin is a collision.
    bool Register(std::shared_ptr<const PermissionModel> model);

    // Binds every registered model to one preset's slices. `global_slices` is
    // permissions.json's `plugins` object, `preset_slices` the preset's
    // `plugin_permissions` object; a model's slice is the member under its
    // name (absent -> null). Throws what a model's BuildLayer throws, prefixed
    // with the model's name.
    PermissionModels Bind(const nlohmann::json& global_slices, const nlohmann::json& preset_slices,
                          bool preset_trusted) const;

    // Startup check: every statically registered tool (ToolRegistry
    // attributions) is claimed by at most one model, and only by a model its
    // own plugin registered. One message per violation; empty when clean.
    std::vector<std::string> ValidateClaims(const ToolRegistry& tools) const;
};

// --- The MCP model (in core until the MCP client leaves core) ----------------
//
// Claims every mcp__<server>__<tool> name and replaces the base stage with the
// server-scoped mode (McpPermissionPolicy) refined by per-tool rules on the
// full name -- resolve_mcp_tool_mode, unchanged. Its layer is an
// McpPermissionPolicy. While MCP is core it is not in the registry: sessions
// bind it directly from AgentPreset::mcp_permissions (BindMcpPermissionModel).
// Its BuildLayer slices -- global {"default": "<mode>"}, preset
// {"servers": {"<server>": "<mode>"}} -- are what the legacy keys
// (permissions.json mcp_default_mode, preset mcp_permissions) become once it
// is registered by the MCP plugin.
inline constexpr std::string_view kMcpPermissionModelName = "mcp";

std::shared_ptr<const PermissionModel> MakeMcpPermissionModel();

// Binds the MCP model to an already-built policy -- the form sessions and
// tests that still hold an McpPermissionPolicy use.
BoundPermissionModel BindMcpPermissionModel(McpPermissionPolicy policy);

// The McpPermissionPolicy a bound model set carries for MCP, or null when the
// set has no MCP binding.
const McpPermissionPolicy* FindMcpPolicy(const PermissionModels& models);

}  // namespace minimal::tools
