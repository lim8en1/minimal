#pragma once

#include <string>
#include <string_view>

namespace minimal::tools_web {

// A hand-written HTML -> text extraction, not a new vcpkg dependency for a
// tool that just needs readable prose out of a page. Strips <script>/<style>
// content wholesale, turns block-level elements into line breaks, decodes
// the common HTML entities, and collapses runs of whitespace.
//
// Deliberately not a full HTML parser: no DOM, no malformed-markup recovery
// beyond "don't crash and don't hang". A mis-rendered edge case here is
// cosmetic (worse prose for one page), unlike the bash command-scanner
// precedent that graduated to tree-sitter-bash -- that scanner's hand-rolled
// failure mode was a security one (disclosing fewer commands than a
// permission decision needed); this one's is not. If mis-parses prove
// common enough to matter, a real HTML library is the natural follow-up.
std::string HtmlToText(std::string_view html);

}  // namespace minimal::tools_web
