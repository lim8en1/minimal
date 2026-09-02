#pragma once

// Turns a keybindings.json key spec ("ctrl+shift+tab", "alt+j", "f5", "q")
// into every ftxui::Event a terminal might actually send for it.
//
// Before this, app.cpp's KeyToEvents was a closed if-ladder over twelve
// literal key names (ARCHITECTURE.md/the customization surface promises
// keybindings.json as real remapping precisely BECAUSE terminals disagree on
// what they can encode -- "write user config" only means something if the
// config can name more than twelve keys). Anything else -- ctrl+p, f2,
// alt+j -- matched nothing; the startup warning ("no key mapping for
// 'action' -> 'key'") was honest about the gap but did not close it.
//
// grammar: key := modifier* base
//   modifier := "ctrl" | "alt" | "shift", joined by '+', any order, repeats
//               ignored
//   base     := a single a-z/0-9 character, an F-key ("f1".."f12"), or a
//               named key (see the .cpp for the full table)
//
// Terminal encoding is genuinely ambiguous for some combinations without the
// kitty keyboard protocol / CSI-u (this is exactly why keybindings.json
// exists as user config rather than a fixed table) -- ParseKeySpec returns
// every candidate encoding it knows of for the spec, and the caller matches
// any of them (see MatchesAny in app.cpp). An unrecognized spec returns an
// empty vector, the same "no mapping" signal as before.

#include <string>
#include <vector>

#include <ftxui/component/event.hpp>

namespace minimal::client {

std::vector<ftxui::Event> ParseKeySpec(const std::string& spec);

}  // namespace minimal::client
