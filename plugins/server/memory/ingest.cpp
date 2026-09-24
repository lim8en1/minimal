#include "ingest.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>

#include "minimal/common/utf8.hpp"

namespace minimal::memory {
namespace {

constexpr std::size_t kTitleMaxChars = 80;
constexpr std::size_t kDescriptionMaxChars = 200;

std::string Trim(std::string_view text) {
    std::size_t start = 0, end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(start, end - start));
}

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        std::string line(text.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                                          : end - start));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return lines;
}

// A fence opener/closer: up to three spaces, then >= 3 backticks or tildes.
// Returns the marker run ("```", "~~~~") or "" when the line is not a fence.
std::string FenceMarker(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    if (i >= line.size() || (line[i] != '`' && line[i] != '~')) return {};
    const char c = line[i];
    std::size_t j = i;
    while (j < line.size() && line[j] == c) ++j;
    return j - i >= 3 ? line.substr(i, j - i) : std::string();
}

// ATX heading: up to three spaces, 1-6 '#', then a space or end of line.
// Returns the level (0 = not a heading) and the heading text.
int HeadingLevel(const std::string& line, std::string& text) {
    std::size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    std::size_t j = i;
    while (j < line.size() && line[j] == '#') ++j;
    const int level = static_cast<int>(j - i);
    if (level < 1 || level > 6) return 0;
    if (j < line.size() && line[j] != ' ' && line[j] != '\t') return 0;
    std::string rest = Trim(std::string_view(line).substr(j));
    // Optional closing sequence: "## Title ##".
    while (!rest.empty() && rest.back() == '#') rest.pop_back();
    text = Trim(rest);
    return level;
}

struct FrontMatter {
    std::string title;
    std::string description;
};

std::string Unquote(std::string value) {
    value = Trim(value);
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
        value.back() == value.front()) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

// Strips a leading "---" block and reads `title:` / `description:` from it.
std::vector<std::string> TakeFrontMatter(std::vector<std::string> lines, FrontMatter& out) {
    if (lines.empty() || Trim(lines[0]) != "---") return lines;
    std::size_t end = 1;
    while (end < lines.size() && Trim(lines[end]) != "---") ++end;
    if (end >= lines.size()) return lines;  // unterminated: not front matter
    for (std::size_t i = 1; i < end; ++i) {
        const auto colon = lines[i].find(':');
        if (colon == std::string::npos) continue;
        const std::string key = Trim(std::string_view(lines[i]).substr(0, colon));
        const std::string value = Unquote(lines[i].substr(colon + 1));
        if (key == "title") out.title = value;
        else if (key == "description") out.description = value;
    }
    return std::vector<std::string>(lines.begin() + static_cast<std::ptrdiff_t>(end) + 1, lines.end());
}

struct Section {
    std::string title;
    std::vector<std::string> lines;
};

std::string Join(const std::vector<std::string>& lines, std::size_t from, std::size_t to) {
    std::string out;
    for (std::size_t i = from; i < to; ++i) {
        out += lines[i];
        out += '\n';
    }
    return out;
}

// Paragraph split outside fences, greedily packed up to `max_bytes`.
std::vector<std::string> SplitOversized(const std::vector<std::string>& lines,
                                        std::size_t max_bytes) {
    // Paragraph boundaries: blank lines that are not inside a fence.
    std::vector<std::pair<std::size_t, std::size_t>> paragraphs;
    std::string fence;
    std::size_t start = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string marker = FenceMarker(lines[i]);
        if (!marker.empty()) {
            if (fence.empty()) fence = marker;
            else if (marker[0] == fence[0] && marker.size() >= fence.size()) fence.clear();
        }
        if (fence.empty() && Trim(lines[i]).empty()) {
            if (i > start) paragraphs.emplace_back(start, i);
            start = i + 1;
        }
    }
    if (start < lines.size()) paragraphs.emplace_back(start, lines.size());

    std::vector<std::string> parts;
    std::string current;
    auto flush = [&] {
        if (!Trim(current).empty()) parts.push_back(current);
        current.clear();
    };
    for (const auto& [from, to] : paragraphs) {
        const std::string paragraph = Join(lines, from, to);
        if (paragraph.size() > max_bytes) {
            // Still too big: line boundaries, then (a single enormous line)
            // a UTF-8-safe byte cut.
            flush();
            for (std::size_t i = from; i < to; ++i) {
                std::string line = lines[i] + "\n";
                while (line.size() > max_bytes) {
                    const std::string head = common::TruncateUtf8Bytes(line, max_bytes);
                    if (!current.empty()) flush();
                    parts.push_back(head);
                    line.erase(0, head.size());
                }
                if (current.size() + line.size() > max_bytes) flush();
                current += line;
            }
            flush();
            continue;
        }
        const std::size_t joined = current.empty() ? paragraph.size() : current.size() + 1 + paragraph.size();
        if (joined > max_bytes) flush();
        if (!current.empty()) current += '\n';
        current += paragraph;
    }
    flush();
    return parts;
}

std::string Breadcrumb(const std::vector<std::string>& stack) {
    std::string out;
    for (const auto& part : stack) {
        if (part.empty()) continue;
        if (!out.empty()) out += " > ";
        out += part;
    }
    return out;
}

}  // namespace

std::string FirstSentence(const std::string& text, std::size_t max_chars) {
    std::string fence;
    for (const auto& raw : SplitLines(text)) {
        const std::string marker = FenceMarker(raw);
        if (!marker.empty()) {
            if (fence.empty()) fence = marker;
            else if (marker[0] == fence[0] && marker.size() >= fence.size()) fence.clear();
            continue;
        }
        if (!fence.empty()) continue;
        std::string line = Trim(raw);
        std::string heading;
        if (line.empty() || HeadingLevel(line, heading) > 0) continue;
        // Strip list, quote and numbering markers.
        for (bool stripped = true; stripped && !line.empty();) {
            stripped = false;
            if (line[0] == '>' || ((line[0] == '-' || line[0] == '*' || line[0] == '+') &&
                                   line.size() > 1 && line[1] == ' ')) {
                line = Trim(std::string_view(line).substr(1));
                stripped = true;
            } else if (std::isdigit(static_cast<unsigned char>(line[0]))) {
                std::size_t i = 0;
                while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
                if (i < line.size() && (line[i] == '.' || line[i] == ')') && i + 1 < line.size() &&
                    line[i + 1] == ' ') {
                    line = Trim(std::string_view(line).substr(i + 1));
                    stripped = true;
                }
            }
        }
        if (line.empty()) continue;
        // Sentence end: ". ", "! ", "? " or the CJK full stop.
        std::size_t cut = std::string::npos;
        for (std::size_t i = 0; i + 1 < line.size(); ++i) {
            if ((line[i] == '.' || line[i] == '!' || line[i] == '?') && line[i + 1] == ' ') {
                cut = i + 1;
                break;
            }
        }
        if (const auto cjk = line.find("\xE3\x80\x82"); cjk != std::string::npos && cjk + 3 < cut) {
            cut = cjk + 3;
        }
        if (cut != std::string::npos) line = line.substr(0, cut);
        return common::TruncateCodePoints(Trim(line), max_chars);
    }
    return {};
}

std::vector<IngestChunk> ChunkMarkdown(const std::string& content, const std::string& fallback_title,
                                       const std::string& fallback_description,
                                       std::size_t max_chunk_bytes) {
    FrontMatter front;
    const auto lines = TakeFrontMatter(SplitLines(content), front);
    if (front.description.empty()) front.description = fallback_description;
    const std::string doc_title = !front.title.empty() ? front.title : fallback_title;

    std::vector<Section> sections;
    sections.push_back({doc_title, {}});  // preamble
    std::vector<std::string> stack(6);
    std::string fence;
    for (const auto& line : lines) {
        const std::string marker = FenceMarker(line);
        if (!marker.empty()) {
            if (fence.empty()) fence = marker;
            else if (marker[0] == fence[0] && marker.size() >= fence.size()) fence.clear();
            sections.back().lines.push_back(line);
            continue;
        }
        std::string heading;
        const int level = fence.empty() ? HeadingLevel(line, heading) : 0;
        if (level == 0) {
            sections.back().lines.push_back(line);
            continue;
        }
        stack[static_cast<std::size_t>(level - 1)] = heading;
        for (std::size_t i = static_cast<std::size_t>(level); i < stack.size(); ++i) stack[i].clear();
        sections.push_back({Breadcrumb(stack), {}});
    }

    std::vector<IngestChunk> chunks;
    for (const auto& section : sections) {
        std::string body = Join(section.lines, 0, section.lines.size());
        if (Trim(body).empty()) continue;  // heading-only: lives on in the breadcrumbs
        std::string title = section.title.empty() ? doc_title : section.title;
        if (body.size() <= max_chunk_bytes) {
            chunks.push_back({title, {}, Trim(body)});
            continue;
        }
        const auto parts = SplitOversized(section.lines, max_chunk_bytes);
        for (std::size_t i = 0; i < parts.size(); ++i) {
            chunks.push_back({title + " (part " + std::to_string(i + 1) + "/" +
                                  std::to_string(parts.size()) + ")",
                              {}, Trim(parts[i])});
        }
    }
    if (chunks.empty()) throw std::runtime_error("memory_store: the file has no content to store");

    for (auto& chunk : chunks) {
        chunk.title = common::TruncateCodePoints(chunk.title.empty() ? "(untitled)" : chunk.title,
                                                 kTitleMaxChars);
        chunk.description = FirstSentence(chunk.body, kDescriptionMaxChars);
        if (chunk.description.empty() || (chunks.size() == 1 && !front.description.empty())) {
            chunk.description = !front.description.empty()
                                    ? common::TruncateCodePoints(front.description, kDescriptionMaxChars)
                                    : chunk.title;
        }
    }
    return chunks;
}

}  // namespace minimal::memory
