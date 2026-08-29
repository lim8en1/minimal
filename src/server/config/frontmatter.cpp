#include "minimal/config/frontmatter.hpp"

#include <stdexcept>

namespace minimal::config {
namespace {

bool is_delimiter_line(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line == "---";
}

}  // namespace

ParsedFrontmatter parse_frontmatter_markdown(std::string_view contents) {
    ParsedFrontmatter result;

    const std::size_t first_nl = contents.find('\n');
    const std::string_view first_line =
        contents.substr(0, first_nl == std::string_view::npos ? contents.size() : first_nl);
    if (!is_delimiter_line(first_line)) {
        result.body.assign(contents);
        return result;
    }

    const std::size_t body_start = (first_nl == std::string_view::npos) ? contents.size() : first_nl + 1;
    std::size_t search_from = body_start;
    while (true) {
        const std::size_t nl = contents.find('\n', search_from);
        const std::string_view line =
            contents.substr(search_from, (nl == std::string_view::npos ? contents.size() : nl) - search_from);

        if (is_delimiter_line(line)) {
            const std::string_view yaml_block = contents.substr(body_start, search_from - body_start);
            result.frontmatter = YAML::Load(std::string(yaml_block));
            const std::size_t remainder_start = (nl == std::string_view::npos) ? contents.size() : nl + 1;
            result.body.assign(contents.substr(remainder_start));
            return result;
        }
        if (nl == std::string_view::npos) {
            throw std::runtime_error("unterminated frontmatter block: no closing '---'");
        }
        search_from = nl + 1;
    }
}

}  // namespace minimal::config
