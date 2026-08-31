#pragma once

// The status bar: a format string of segments ("{version} | {model} | ...")
// resolved from values the app supplies. Segment ids are the E5 registry's
// address space; E4 ships the core ids. An id nobody provides renders NOTHING
// -- and RunTui warns at startup, once, naming the id (the keybindings.json
// rule for names unknown at compile time).

#include <map>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/status_format.hpp"

namespace minimal::client {

// The segment ids this build can fill. The startup check against a user's
// format string is done against this set.
bool IsKnownStatusSegment(std::string_view id);

// Warned ids: format segments no provider exists for. RunTui reports them once
// at startup rather than every frame.
std::vector<std::string> UnknownStatusSegments(const std::string& format);

ftxui::Element RenderStatusBar(const std::vector<StatusSegment>& format,
                               const std::map<std::string, std::string>& values,
                               const Theme& theme);

}  // namespace minimal::client
