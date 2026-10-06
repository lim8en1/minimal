#include "minimal/client/last_selection.hpp"

#include <nlohmann/json.hpp>

#include "minimal/client/config_file_io.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::client {
namespace {

nlohmann::json ReadDoc(const std::filesystem::path& file) {
    std::string text;
    if (!TryReadConfigFile(file, &text)) return nlohmann::json::object();
    auto doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) return nlohmann::json::object();
    return doc;
}

void WriteDoc(const std::filesystem::path& file, const nlohmann::json& doc) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (!WriteConfigFileAtomic(file, doc.dump(2) + "\n")) {
        common::DefaultLogger().warn("could not write {} -- the last pick is not remembered",
                                     file.string());
    }
}

// The string field, or "" when absent or not a string.
std::string Field(const nlohmann::json& entry, const char* key) {
    if (!entry.is_object()) return {};
    const auto it = entry.find(key);
    return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// nullopt leaves the field; a value (an empty model included: "the
// default" is a pick of its own) sets it.
void Apply(nlohmann::json& entry, const char* key, const std::optional<std::string>& value) {
    if (!value) return;
    if (!entry.is_object()) entry = nlohmann::json::object();
    entry[key] = *value;
}

void Erase(nlohmann::json& entry, const char* key) {
    if (entry.is_object()) entry.erase(key);
}

}  // namespace

LastSelectionStore::LastSelectionStore(std::filesystem::path file) : file_(std::move(file)) {}

LastSelection LastSelectionStore::For(const std::string& project_root) const {
    const auto doc = ReadDoc(file_);
    const nlohmann::json* project = nullptr;
    if (const auto projects = doc.find("projects");
        projects != doc.end() && projects->is_object() && !project_root.empty()) {
        if (const auto it = projects->find(project_root); it != projects->end()) project = &*it;
    }
    const nlohmann::json* global = nullptr;
    if (const auto it = doc.find("global"); it != doc.end()) global = &*it;
    // Per field: the project's own pick when it made one (an explicit ""
    // model included -- "the default" is a pick), else the global one.
    const auto pick = [&](const char* key) {
        if (project != nullptr && project->is_object() && project->contains(key)) {
            return Field(*project, key);
        }
        return global != nullptr ? Field(*global, key) : std::string();
    };
    return {pick("agent"), pick("model")};
}

void LastSelectionStore::Record(const std::string& project_root,
                                const std::optional<std::string>& agent,
                                const std::optional<std::string>& model) const {
    if (!agent && !model) return;
    auto doc = ReadDoc(file_);
    if (!project_root.empty()) {
        if (!doc["projects"].is_object()) doc["projects"] = nlohmann::json::object();
        auto& entry = doc["projects"][project_root];
        Apply(entry, "agent", agent);
        Apply(entry, "model", model);
    }
    Apply(doc["global"], "agent", agent);
    Apply(doc["global"], "model", model);
    WriteDoc(file_, doc);
}

void LastSelectionStore::Forget(const std::string& project_root, bool agent, bool model) const {
    if (!agent && !model) return;
    auto doc = ReadDoc(file_);
    for (auto* entry : {project_root.empty() || !doc["projects"].is_object()
                            ? static_cast<nlohmann::json*>(nullptr)
                            : &doc["projects"][project_root],
                        &doc["global"]}) {
        if (entry == nullptr) continue;
        if (agent) Erase(*entry, "agent");
        if (model) Erase(*entry, "model");
    }
    WriteDoc(file_, doc);
}

}  // namespace minimal::client
