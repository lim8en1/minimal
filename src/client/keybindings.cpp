#include "minimal/client/keybindings.hpp"

#include <fstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace minimal::client {

namespace {

// The shipped defaults (bundled/keybindings.json seeds the same content). Every
// name here is a core action; the file may add any others.
const std::unordered_map<std::string, std::string>& DefaultBindings() {
    static const std::unordered_map<std::string, std::string> kDefaults{
        {"tab.next", "ctrl+tab"},
        {"tab.previous", "ctrl+shift+tab"},
        {"group.previous", "alt+left"},
        {"group.next", "alt+right"},
        {"jump.attention", "alt+a"},
        {"session.picker", "ctrl+o"},
        {"panel.toggle", "ctrl+t"},
        {"tab.close", "ctrl+w"},
        {"chat.load_older", "pageup"},
        {"app.quit", "ctrl+q"},
    };
    return kDefaults;
}

}  // namespace

Keybindings Keybindings::Parse(std::string_view json) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("keybindings.json is not valid JSON: ") + e.what());
    }
    if (!doc.is_object()) {
        throw std::runtime_error("keybindings.json must be a JSON object of action -> key");
    }

    Keybindings out;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (!it.value().is_string()) {
            throw std::runtime_error("keybindings.json: value for '" + it.key() +
                                     "' must be a string");
        }
        // Unknown action names are KEPT: a client plugin (E5) registers views
        // addressable from here by runtime-chosen ids, and this file may predate
        // or postdate any given build. See the header comment.
        out.bindings_[it.key()] = it.value().get<std::string>();
    }
    return out;
}

Keybindings Keybindings::Load(const std::filesystem::path& config_dir) {
    const auto path = config_dir / "keybindings.json";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        // A fresh config dir is the normal first-run state: the defaults stand.
        Keybindings out;
        out.bindings_ = DefaultBindings();
        return out;
    }
    std::ifstream in(path);
    if (!in) throw std::runtime_error("keybindings.json exists but cannot be read: " + path.string());
    const std::string contents((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    Keybindings parsed = Parse(contents);
    // Defaults fill actions the user's file does not mention, so an upgrade that
    // adds a core action still works without rewriting anyone's file. The user's
    // own binding always wins over the default for the same action.
    for (const auto& [action, key] : DefaultBindings()) {
        parsed.bindings_.emplace(action, key);
    }
    return parsed;
}

std::string Keybindings::KeyFor(std::string_view action, std::string_view fallback) const {
    if (const auto it = bindings_.find(std::string(action)); it != bindings_.end()) {
        return it->second;
    }
    return std::string(fallback);
}

bool Keybindings::EnsureDefault(std::string_view action, std::string_view key) {
    if (key.empty()) return false;
    const std::string name(action);
    if (bindings_.find(name) != bindings_.end()) return false;
    bindings_.emplace(name, std::string(key));
    return true;
}

}  // namespace minimal::client
