#pragma once

#include <cstddef>
#include <string_view>

namespace minimal::llm {

// Cheap pre-parse nesting guard for wire JSON. nlohmann's parser is recursive
// descent with no depth limit: ~1 MiB of '[' (the SSE frame cap) reaches a
// nesting depth that overflows the call stack -- a crash reachable from any
// hostile or broken gateway. Callers parse only when this returns true and
// classify the refusal as a parse error; parse() itself still validates the
// document, so this is purely a resource guard (counting opens, not
// validating brace matching).
//
// max_depth 512 is far above any legitimate model/tool payload (tool
// arguments, stream chunks, error bodies are all single-digit-depth objects).
inline bool JsonDepthWithinLimit(std::string_view text, std::size_t max_depth = 512) {
    std::size_t opens = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        switch (c) {
            case '"': in_string = true; break;
            case '{':
            case '[':
                if (++opens > max_depth) return false;
                break;
            default: break;
        }
    }
    return true;
}

}  // namespace minimal::llm
