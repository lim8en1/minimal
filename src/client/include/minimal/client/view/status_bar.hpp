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

// One resolved literal-or-value piece, in render order.
struct StatusBarPart {
    bool is_literal = false;
    std::string text;
};

// The status bar's width decision, pure and independently testable
// (ARCHITECTURE.md "Testing": render output itself is not unit-tested, the
// logic behind it is). Resolves `format` + `values` into the final ordered
// parts, given the terminal width available in display columns
// (`max_width <= 0` means "don't clamp", e.g. an unknown terminal size).
//
// `{session}` is the ONLY elastic segment: capped at min(24, leftover
// budget) columns, truncated with an ellipsis, or dropped entirely (along
// with its now-orphaned adjacent literal) when the OTHER segments alone
// already exceed max_width. Every other segment renders at its natural
// width, unclamped -- a status value silently losing digits (a truncated
// token count reading as a smaller, WRONG number) is worse than a status bar
// that overflows the terminal.
std::vector<StatusBarPart> ResolveStatusBar(const std::vector<StatusSegment>& format,
                                            const std::map<std::string, std::string>& values,
                                            int max_width);

ftxui::Element RenderStatusBar(const std::vector<StatusSegment>& format,
                               const std::map<std::string, std::string>& values,
                               const Theme& theme, int max_width);

}  // namespace minimal::client
