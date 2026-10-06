#include "memory_runtime.hpp"

#include <exception>

#include "minimal/agent/session_support_service.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/plugins/service_registry.hpp"

namespace minimal::memory {

std::shared_ptr<MemoryRuntime> BuildMemoryRuntime(const nlohmann::json& config,
                                                  const std::filesystem::path& data_dir,
                                                  plugins::ServiceRegistry* services,
                                                  std::string& error) {
    try {
        auto runtime = std::make_shared<MemoryRuntime>();
        runtime->live = std::make_shared<common::LiveValue<MemorySettings>>(ParseMemorySettings(config));
        runtime->store = std::make_unique<MemoryStore>(data_dir / "memory.sqlite3");

        // core.secrets is core-provided before any plugin loads.
        const auto* secrets =
            services != nullptr ? static_cast<const config::SecretResolver*>(
                                      services->Find(config::kSecretsServiceName))
                                : nullptr;
        auto plan = MakeEmbeddingPlan(*runtime->Settings(), data_dir, secrets);
        // The one re-embed trigger: rows embedded by a different backend or
        // model go back to pending. With no backend configured, vectors are
        // left alone -- re-enabling the same backend later finds them intact.
        if (plan.factory) runtime->store->ResetStaleSignatures(plan.signature);
        runtime->embeddings = std::make_unique<EmbeddingService>(
            runtime->store.get(), std::move(plan), common::DefaultLogger());

        // Resolved lazily on the tagger's worker, by which time core services
        // are wired (the worker starts on first use, never at registration).
        const std::string model_ref = runtime->Settings()->tagger_model;
        Tagger::Resolver resolver = [services, model_ref]() -> std::optional<TaggerLlm> {
            if (services == nullptr) return std::nullopt;
            void* raw = services->Find(agent::kSessionSupportServiceName);
            if (raw == nullptr) return std::nullopt;
            auto* support = static_cast<agent::SessionSupportService*>(raw);
            auto resolved = support->ResolveModelRef(model_ref);
            if (!resolved) return std::nullopt;
            return TaggerLlm{resolved->provider, resolved->model};
        };
        runtime->tagger = std::make_unique<Tagger>(runtime->live, runtime->store.get(),
                                                   runtime->embeddings.get(), std::move(resolver),
                                                   common::DefaultLogger());
        return runtime;
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

}  // namespace minimal::memory
