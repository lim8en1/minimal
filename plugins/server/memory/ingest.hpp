#pragma once

// Markdown -> memory entries, for memory_store(path=...).
//
// Chunking rules:
//   - A chunk is a heading section. Headings inside fenced code blocks (```
//     or ~~~) are code, not structure -- a "# comment" in a shell snippet
//     does not split anything.
//   - A chunk's title is its heading BREADCRUMB ("Deploy > Staging"), so a
//     chunk stays self-describing when read alone.
//   - Text before the first heading is its own chunk, titled after the
//     document (front matter title, else the file name).
//   - A heading with no body of its own contributes only to its children's
//     breadcrumbs.
//   - A section over the size cap is split on blank lines (never inside a
//     fence); a single paragraph that still does not fit is split on line
//     boundaries.
//   - Optional YAML-ish front matter (--- ... ---) may set `title` and
//     `description`. Nothing else is read from it: tags are the tagger's job.
//   - Each chunk's description is its first sentence (the document
//     description when the chunk has none, or when there is only one chunk).

#include <cstddef>
#include <string>
#include <vector>

namespace minimal::memory {

struct IngestChunk {
    std::string title;
    std::string description;
    std::string body;
};

// `fallback_title` / `fallback_description` name and describe the document
// when its front matter does not (the caller passes the tool arguments, else
// the file name). Throws std::runtime_error when nothing ingestible remains.
std::vector<IngestChunk> ChunkMarkdown(const std::string& content,
                                       const std::string& fallback_title,
                                       const std::string& fallback_description,
                                       std::size_t max_chunk_bytes);

// First sentence of a block of markdown, list/quote markers stripped, at most
// `max_chars` code points. Empty when the text has no prose line.
std::string FirstSentence(const std::string& text, std::size_t max_chars);

}  // namespace minimal::memory
