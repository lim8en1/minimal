#pragma once

// Every process-wide registry plugins fill and core reads, in one place.
//
// main() builds the registries once and hands this struct -- by value; it is
// just pointers -- to each consumer's constructor (the plugin loader, the
// session registry, the RPC service), instead of threading each registry
// through its own constructor argument or post-construction setter. A null
// pointer means "this host does not offer that seam" (tests leave most of
// them null); consumers treat it exactly as they treated a null argument.
//
// The registries outlive every consumer (process lifetime, see main.cpp).

namespace minimal::tools {
class ToolRegistry;
class PermissionAdvisorRegistry;
}  // namespace minimal::tools
namespace minimal::llm {
class ProviderFactoryRegistry;
class ModelCatalogRegistry;
class MetadataCatalogRegistry;
class VariantParamsRegistry;
}  // namespace minimal::llm
namespace minimal::commands {
class CommandRegistry;
}  // namespace minimal::commands
namespace minimal::agent {
class SessionObserverRegistry;
class CompactionStrategyRegistry;
class ContextContributorRegistry;
class MentionResolverRegistry;
}  // namespace minimal::agent
namespace minimal::rpc {
class AuthProviderRegistry;
}  // namespace minimal::rpc
namespace minimal::hooks {
class HookDispatcher;
}  // namespace minimal::hooks

namespace minimal::plugins {

class ServiceRegistry;

struct HostRegistries {
    tools::ToolRegistry* tools = nullptr;
    llm::ProviderFactoryRegistry* providers = nullptr;
    commands::CommandRegistry* commands = nullptr;
    agent::SessionObserverRegistry* observers = nullptr;
    rpc::AuthProviderRegistry* auth_providers = nullptr;
    tools::PermissionAdvisorRegistry* permission_advisors = nullptr;
    llm::ModelCatalogRegistry* model_catalogs = nullptr;
    llm::MetadataCatalogRegistry* metadata_catalogs = nullptr;
    llm::VariantParamsRegistry* variant_params = nullptr;
    agent::CompactionStrategyRegistry* compaction_strategies = nullptr;
    agent::ContextContributorRegistry* context_contributors = nullptr;
    agent::MentionResolverRegistry* mention_resolvers = nullptr;
    ServiceRegistry* services = nullptr;
    // The hook mechanism: plugins register points here; sessions emit through it.
    hooks::HookDispatcher* hooks = nullptr;
};

}  // namespace minimal::plugins
