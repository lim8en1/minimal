#include "minimal/config/provider_config_extensions.hpp"

#include <algorithm>
#include <utility>

#include "minimal/common/logger.hpp"

namespace minimal::config {

void ProviderConfigExtensions::AddTranslator(
    std::shared_ptr<const ProviderEntryTranslator> translator) {
    if (translator) translators_.push_back({attribution_, std::move(translator)});
}

void ProviderConfigExtensions::AddSecretSource(std::shared_ptr<const SecretSource> source) {
    if (!source) return;
    if (attribution_.empty()) {
        common::DefaultLogger().warn(
            "secret source '{}' registered outside a plugin's registration; ignored -- its "
            "namespace is the plugin's name",
            source->Describe());
        return;
    }
    if (FindSecretSource(attribution_) != nullptr) {
        common::DefaultLogger().warn("plugin '{}' registered a second secret source; ignored",
                                     attribution_);
        return;
    }
    sources_.push_back({attribution_, std::move(source)});
}

void ProviderConfigExtensions::Translate(nlohmann::json& entry, const std::string& where) const {
    for (const auto& translator : translators_) translator.value->Translate(entry, where);
}

const SecretSource* ProviderConfigExtensions::FindSecretSource(
    const std::string& plugin_name) const {
    for (const auto& source : sources_) {
        if (source.plugin == plugin_name) return source.value.get();
    }
    return nullptr;
}

void ProviderConfigExtensions::RemoveByAttribution(const std::string& plugin_name) {
    const auto owned = [&](const auto& entry) { return entry.plugin == plugin_name; };
    translators_.erase(std::remove_if(translators_.begin(), translators_.end(), owned),
                       translators_.end());
    sources_.erase(std::remove_if(sources_.begin(), sources_.end(), owned), sources_.end());
}

std::vector<std::string> ProviderConfigExtensions::OwnedBy(const std::string& plugin_name) const {
    std::vector<std::string> out;
    for (const auto& entry : translators_) {
        if (entry.plugin == plugin_name) out.push_back("providers.json translator");
    }
    for (const auto& entry : sources_) {
        if (entry.plugin == plugin_name) out.push_back("secrets: " + entry.value->Describe());
    }
    return out;
}

}  // namespace minimal::config
