#pragma once

#include <string>
#include <string_view>

#include <yaml-cpp/yaml.h>

namespace minimal::config {

struct ParsedFrontmatter {
    YAML::Node frontmatter;  // IsNull() if the file had no frontmatter block at all
    std::string body;
};

// Splits `contents` into a YAML frontmatter block and a Markdown body, per the
// agents/*.md, skills/*.md, prompts/*.md convention: frontmatter for structured
// metadata, Markdown body loaded verbatim.
//
// A frontmatter block must open the file with a "---" line; if it does not, the
// whole input is treated as body with no frontmatter node. Throws
// std::runtime_error if a frontmatter block is opened but never closed with a
// second "---" line.
ParsedFrontmatter parse_frontmatter_markdown(std::string_view contents);

}  // namespace minimal::config
