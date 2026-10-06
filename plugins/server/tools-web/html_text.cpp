#include "html_text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "minimal/common/utf8.hpp"

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

// The tag-ending `>`, skipping over quoted attribute values: `<a title="a>b">`
// has a `>` inside the quoted value that is not the tag's own end. A naive
// `find('>', start)` stops at the first one, leaking `b">` into the output as
// ordinary text. `start` is the position of `<`.
std::size_t FindTagEnd(std::string_view html, std::size_t start) {
    std::size_t i = start + 1;
    char quote = '\0';
    for (; i < html.size(); ++i) {
        if (quote != '\0') {
            if (html[i] == quote) quote = '\0';
            continue;
        }
        if (html[i] == '"' || html[i] == '\'') {
            quote = html[i];
        } else if (html[i] == '>') {
            return i;
        }
    }
    return std::string_view::npos;
}

const std::unordered_set<std::string>& BlockTags() {
    static const std::unordered_set<std::string> tags = {
        "p",  "div",  "br",  "li",     "h1",     "h2",     "h3",     "h4",
        "h5", "h6",   "tr",  "table",  "ul",     "ol",     "section", "article",
        "header", "footer", "blockquote", "pre", "hr"};
    return tags;
}

void AppendEntityCodepoint(std::string& out, std::uint32_t codepoint) {
    // Surrogates (UTF-16 code units with no Unicode scalar value) and C0
    // control characters never belong in the text: a numeric entity naming
    // them produced INVALID UTF-8 (D800-DFFF as 3-byte sequences) or a raw
    // NUL byte (&#0;) -- the first corrupts every downstream UTF-8 consumer,
    // the second truncates C-string handling. Both are dropped; the model
    // loses nothing it could have used. Surrogates and out-of-range
    // codepoints have no encoding, so AppendUtf8 drops them -- cosmetic, not
    // a correctness boundary (see html_text.hpp's header comment).
    if (codepoint < ' ' && codepoint != '\t' && codepoint != '\n' && codepoint != '\r') {
        return;
    }
    common::AppendUtf8(out, codepoint);
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
    // the ';' search must be bounded to that window -- searching the SUBSTR,
    // not the comment's promise alone: `html.find(';', pos + 1)` over the
    // full string scans to end-of-document per '&' when none is found,
    // O(N^2) on a page of bare '&' characters (measured hours of CPU on an
    // 8 MiB body). Only comparing the result against window_end afterward,
    // as this used to do, bounds which matches are ACCEPTED, not how far the
    // search itself looks.
    const std::size_t window_end = std::min(pos + 13, html.size());
    const std::size_t semi_in_window = html.substr(pos + 1, window_end - pos - 1).find(';');
    if (semi_in_window == std::string_view::npos) {
        out += '&';
        return 1;
    }
    const std::size_t semi = pos + 1 + semi_in_window;
    const std::string_view body = html.substr(pos + 1, semi - pos - 1);
    if (!body.empty() && body[0] == '#') {
        const bool hex = body.size() > 1 && (body[1] == 'x' || body[1] == 'X');
        const std::string_view digits = body.substr(hex ? 2 : 1);
        if (digits.empty()) {
            out += '&';
            return 1;
        }
        try {
            // `pos` (the consumed-length out-param), not nullptr: stoul does
            // not throw for a PARTIAL parse ("12abc" -> 12, silently
            // swallowing "abc" into the entity's consumed span instead of
            // leaving it as literal text) -- consumed must cover the whole
            // digit run. The value is also bounds-checked before narrowing:
            // "&#x100000041;" fits in stoul's own (wider) return type without
            // throwing, but static_cast<uint32_t> used to wrap it to 'A'
            // instead of rejecting a codepoint past the valid Unicode range.
            std::size_t consumed = 0;
            const unsigned long value = std::stoul(std::string(digits), &consumed, hex ? 16 : 10);
            if (consumed != digits.size() || value > 0x10FFFFUL) {
                out += '&';
                return 1;
            }
            AppendEntityCodepoint(out, static_cast<std::uint32_t>(value));
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
    AppendEntityCodepoint(out, it->second);
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
            // Regression: gating the increment on `line_has_content` alone
            // meant the FIRST newline after real content flipped it false
            // immediately, so a SECOND (or third) consecutive newline in
            // the very same gap was never counted -- pending_newlines could
            // only ever reach 1, and the min(..., 2) cap below was dead
            // code. Counting continues as long as a gap is already open
            // (pending_newlines > 0), not just on the first newline of it;
            // it still never starts counting before any real content, so a
            // leading blank line is not introduced.
            if (line_has_content || pending_newlines > 0) {
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
            // A genuine tag/closing-tag/comment/doctype/processing-instruction
            // starts with a letter, `/`, `!` or `?`; anything else (a stray
            // "x < 5") is a literal less-than, not markup -- treating it as
            // one silently ate everything up to the next unrelated `>`
            // ("if x < 5 then y > 3 done" became "if x 3 done").
            const std::size_t tag_start = i + 1;
            const bool maybe_markup =
                tag_start < n && (std::isalpha(static_cast<unsigned char>(html[tag_start])) ||
                                  html[tag_start] == '/' || html[tag_start] == '!' ||
                                  html[tag_start] == '?');
            if (!maybe_markup) {
                out += '<';
                ++i;
                continue;
            }

            // A comment can contain a literal `>` before its real close
            // ("<!-- note > hidden -->"); only `-->` ends it, never the
            // first `>` a naive scan would find.
            if (MatchesCi(html, i, "<!--")) {
                const std::size_t close = html.find("-->", i + 4);
                i = (close == std::string_view::npos) ? n : close + 3;
                continue;
            }

            const bool closing = html[tag_start] == '/';
            const std::size_t name_start = closing ? tag_start + 1 : tag_start;
            std::size_t name_end = name_start;
            while (name_end < n &&
                   (std::isalnum(static_cast<unsigned char>(html[name_end])) || html[name_end] == '-')) {
                ++name_end;
            }
            const std::string tag_name = ToLowerCopy(html.substr(name_start, name_end - name_start));

            const std::size_t gt = FindTagEnd(html, i);
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
