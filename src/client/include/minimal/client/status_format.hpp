#pragma once

// The status-bar format string: "{version} | {model} | {tokens} | ..." --
// the ordering/selection config for status segments (ARCHITECTURE.md
// "Customization surface", docs/EXTENSIBILITY.md §4.2). E4 ships the core
// segment ids only; the segment REGISTRY that lets a plugin add one is E5 --
// but the parse side is built now, because "an unknown id renders nothing
// with a startup warning" is a property of the format string itself, not of
// who provides segments. Splitting parse from render is what keeps that
// warning testable without FTXUI.

#include <string>
#include <string_view>
#include <vector>

namespace minimal::client {

// One literal-or-segment piece of a parsed format string.
struct StatusSegment {
    bool is_segment = false;  // false: literal text between {..} groups
    std::string text;         // the literal, or the segment id without braces
    // "{id?}": the segment may legitimately be absent (a plugin that is not
    // installed or is disabled) -- it renders nothing either way, but its
    // absence is not warned about at startup. The default format marks the
    // bundled plugins' segments this way.
    bool optional = false;
};

// Parses "{version} | {tokens}" into its pieces. Unbalanced braces are literal
// text: a user writing "{" probably meant it, and inventing an error channel
// for decoration is worse than rendering what they typed.
std::vector<StatusSegment> ParseStatusFormat(std::string_view format);

}  // namespace minimal::client
