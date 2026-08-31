#pragma once

// User keybindings: action name -> key, read from <config>/keybindings.json
// (ARCHITECTURE.md "Customization surface"). Remapping is user config, not a
// nicety: terminals disagree on what they can even encode -- Shift+Enter is
// indistinguishable from Enter without the kitty keyboard protocol / CSI-u --
// so the default newline-in-prompt is the portable Alt+Enter and a user whose
// terminal CAN send Shift+Enter remaps it in this file.
//
// TOLERANCE IS LOAD-BEARING: the file must accept action names nothing in this
// build knows. Client plugins (E5) register views addressable from here by
// their runtime-chosen ids, and a theme shared with a newer build must keep
// loading -- so unknown names are KEPT, not rejected, exactly the theme's
// unknown-role rule. A value that is not a string is a malformed file and
// fails the load, naming the action.

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace minimal::client {

class Keybindings {
public:
    // Parses a keybindings document. Throws std::runtime_error naming the
    // offending action on malformed content.
    static Keybindings Parse(std::string_view json);

    // Loads <config_dir>/keybindings.json. Missing file -> the defaults below
    // (a fresh config dir is a normal state, not an error); malformed file ->
    // throws, naming the path.
    static Keybindings Load(const std::filesystem::path& config_dir);

    // The key for an action, or `fallback` when neither the user's file nor the
    // defaults define one. Every core action HAS a default, so the fallback is
    // for plugin-registered actions a user's older file predates.
    std::string KeyFor(std::string_view action, std::string_view fallback = "") const;

    // The file's own names, including ones nothing in this build binds. What an
    // E5 plugin registry consults before registering its runtime defaults.
    const std::unordered_map<std::string, std::string>& all() const { return bindings_; }

private:
    std::unordered_map<std::string, std::string> bindings_;
};

}  // namespace minimal::client
