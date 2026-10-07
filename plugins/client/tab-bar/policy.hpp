#pragma once

// tab-bar policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

namespace minimal::client::tab_bar {

// "«123 " / " 123»": digits plus the glyph plus a hair of room. Fixed rather
// than measured: a count wider than this is 1000+ tabs.
inline constexpr int kOverflowMarkerWidth = 5;

}  // namespace minimal::client::tab_bar
