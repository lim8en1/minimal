#include "html_text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace minimal::tools_web {

namespace {

std::string ToLowerCopy(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool MatchesCi(std::string_view html, std::size_t pos, std::string_view needle) {
    if (pos + needle.size() > html.size()) return false;
    for (std::size_t k = 0; k < needle.size(); ++k) {
        if (std::tolower(static_cast<unsigned char>(html[pos + k])) !=
            std::tolower(static_cast<unsigned char>(needle[k]))) {
            return false;
        }
    }
    return true;
}

const std::unordered_set<std::string>& BlockTags() {
    static const std::unordered_set<std::string> tags = {
        "p",  "div",  "br",  "li",     "h1",     "h2",     "h3",     "h4",
        "h5", "h6",   "tr",  "table",  "ul",     "ol",     "section", "article",
        "header", "footer", "blockquote", "pre", "hr"};
    return tags;
}

void AppendUtf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out += static_cast<char>(codepoint);
    } else if (codepoint <= 0x7FF) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0x10FFFF) {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    // Out-of-range codepoints are dropped silently -- cosmetic, not a
    // correctness boundary (see html_text.hpp's header comment).
}

const std::unordered_map<std::string, std::uint32_t>& NamedEntities() {
    static const std::unordered_map<std::string, std::uint32_t> entities = {
        {"amp", '&'},   {"lt", '<'},      {"gt", '>'},   {"quot", '"'},
        {"apos", '\''}, {"nbsp", 0xA0u},  {"mdash", 0x2014u}, {"ndash", 0x2013u},
        {"hellip", 0x2026u}, {"copy", 0xA9u}, {"reg", 0xAEu}, {"trade", 0x2122u},
        {"lsquo", 0x2018u}, {"rsquo", 0x2019u}, {"ldquo", 0x201Cu}, {"rdquo", 0x201Du},
    };
    return entities;
}

// Decodes one entity starting at html[pos] == '&'. Returns the number of
// source bytes consumed (at least 1 -- an unrecognized "&..." with no
// terminating ';' within a short window is emitted as a literal '&').
std::size_t DecodeEntity(std::string_view html, std::size_t pos, std::string& out) {
    // An entity is '&' + body + ';' with total length <= 13 from the '&', so
    // the ';' search is bounded to that window. Unbounded, a page of '&'
    // characters scanned to end-of-document per '&' -- O(N^2) on an 8MiB body
    // was hours of CPU. npos compares past the window, so the unbounded miss
    // lands in the same branch.
    const std::size_t window_end = std::min(pos + 13, html.size());
    const std::size_t semi = html.find(';', pos + 1);
    if (semi >= window_end) {
        out += '&';
        return 1;
    }
    const std::string_view body = html.substr(pos + 1, semi - pos - 1);
    if (!body.empty() && body[0] == '#') {
        const bool hex = body.size() > 1 && (body[1] == 'x' || body[1] == 'X');
        const std::string_view digits = body.substr(hex ? 2 : 1);
        if (digits.empty()) {
            out += '&';
            return 1;
        }
        try {
            const std::uint32_t codepoint =
                static_cast<std::uint32_t>(std::stoul(std::string(digits), nullptr, hex ? 16 : 10));
            AppendUtf8(out, codepoint);
            return semi - pos + 1;
        } catch (const std::exception&) {
            out += '&';
            return 1;
        }
    }
    const auto it = NamedEntities().find(ToLowerCopy(body));
    if (it == NamedEntities().end()) {
        out += '&';
        return 1;
    }
    AppendUtf8(out, it->second);
    return semi - pos + 1;
}

// Collapses runs of horizontal whitespace to one space, runs of blank lines
// to at most one, and trims each line -- markup routinely leaves a newline's
// worth of indentation around every tag.
std::string CollapseWhitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    int pending_newlines = 0;
    bool line_has_content = false;

    for (const char c : text) {
        if (c == '\n') {
            if (line_has_content) {
                pending_newlines = std::min(pending_newlines + 1, 2);
            }
            pending_space = false;
            line_has_content = false;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            if (line_has_content) pending_space = true;
            continue;
        }
        if (pending_newlines > 0) {
            out.append(static_cast<std::size_t>(pending_newlines), '\n');
            pending_newlines = 0;
        } else if (pending_space) {
            out += ' ';
        }
        pending_space = false;
        out += c;
        line_has_content = true;
    }
    return out;
}

}  // namespace

std::string HtmlToText(std::string_view html) {
    std::string out;
    out.reserve(html.size() / 2);

    std::size_t i = 0;
    const std::size_t n = html.size();

    while (i < n) {
        if (html[i] == '<') {
            const std::size_t tag_start = i + 1;
            const bool closing = tag_start < n && html[tag_start] == '/';
            const std::size_t name_start = closing ? tag_start + 1 : tag_start;
            std::size_t name_end = name_start;
            while (name_end < n &&
                   (std::isalnum(static_cast<unsigned char>(html[name_end])) || html[name_end] == '-')) {
                ++name_end;
            }
            const std::string tag_name = ToLowerCopy(html.substr(name_start, name_end - name_start));

            const std::size_t gt = html.find('>', i);
            if (gt == std::string_view::npos) break;  // malformed tail; stop rather than hang

            if (!closing && (tag_name == "script" || tag_name == "style")) {
                const std::string close_tag = "</" + tag_name;
                std::size_t close_pos = std::string_view::npos;
                for (std::size_t p = gt + 1; p + close_tag.size() <= n; ++p) {
                    if (MatchesCi(html, p, close_tag)) {
                        close_pos = p;
                        break;
                    }
                }
                if (close_pos == std::string_view::npos) break;  // rest of the doc is script/style
                const std::size_t close_gt = html.find('>', close_pos);
                i = (close_gt == std::string_view::npos) ? n : close_gt + 1;
                continue;
            }

            if (!tag_name.empty() && BlockTags().count(tag_name) != 0) out += '\n';

            i = gt + 1;
            continue;
        }

        if (html[i] == '&') {
            i += DecodeEntity(html, i, out);
            continue;
        }

        out += html[i];
        ++i;
    }

    return CollapseWhitespace(out);
}

}  // namespace minimal::tools_web
