#include "minimal/config/provider_config_extensions.hpp"

#include <algorithm>
#include <utility>

namespace minimal::config {

void ProviderConfigExtensions::AddTranslator(
    std::shared_ptr<const ProviderEntryTranslator> translator) {
    if (translator) translators_.push_back({attribution_, std::move(translator)});
}

void ProviderConfigExtensions::AddCredentialSource(std::shared_ptr<const CredentialSource> source) {
    if (source) sources_.push_back({attribution_, std::move(source)});
}

void ProviderConfigExtensions::Translate(nlohmann::json& entry, const std::string& where) const {
    for (const auto& translator : translators_) translator.value->Translate(entry, where);
}

std::optional<std::string> ProviderConfigExtensions::LookupCredential(
    const std::string& provider_name, std::string& origin) const {
    for (const auto& source : sources_) {
        if (auto key = source.value->Lookup(provider_name)) {
            origin = source.value->Describe();
            return key;
        }
    }
    return std::nullopt;
}

std::vector<std::string> ProviderConfigExtensions::DescribeCredentialSources() const {
    std::vector<std::string> out;
    for (const auto& source : sources_) out.push_back(source.value->Describe());
    return out;
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
        if (entry.plugin == plugin_name) out.push_back("credentials: " + entry.value->Describe());
    }
    return out;
}

}  // namespace minimal::config
