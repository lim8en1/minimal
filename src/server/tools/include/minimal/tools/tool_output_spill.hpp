#pragma once

// Tool output too large for the model's context is cut to a head and a tail,
// and the WHOLE output is spilled to a project-local file the model can page
// through with read or grep: the model reads only text, so "there is more"
// has to be a PATH inside the project root (where read/grep need no prompt),
// not a structured handle. One shape and one note for every tool that cuts
// output -- bash, python, web fetch, and core's global result ceiling.
//
// Layout: <project_root>/.minimal/tool-output/<session_id>/<name>. The
// tool-output dir carries its own `.gitignore` ("*"), scoped to it alone so
// a project that tracks other .minimal/ content is untouched. Each session
// dir keeps only its newest kSpillKeepPerSession files.

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace minimal::tools {

struct ToolContext;

// Project-relative directory of all spills ("" session id: the dir itself).
std::filesystem::path ToolOutputDir(const std::filesystem::path& project_root,
                                    const std::string& session_id);

// The text cut to at most `cap` bytes: the first quarter and the last three
// quarters (in a long log both the first error and the final status matter),
// both cut on UTF-8 boundaries, with an omission marker between them. Text
// within the cap is returned unchanged.
std::string HeadTail(std::string_view text, std::size_t cap);

// A file written incrementally, opened on the first Append -- so a caller can
// hand one to a stream that may never overflow and nothing touches the disk.
// Not thread-safe; one writer per tool call.
class ToolOutputSpill {
public:
    // `name`: a server-chosen file name (never model-supplied); empty picks a
    // unique one.
    ToolOutputSpill(std::filesystem::path project_root, std::string session_id,
                    std::string name);
    // Named after the call's tool_use_id.
    static ToolOutputSpill For(const ToolContext& ctx);

    ToolOutputSpill(ToolOutputSpill&& other) noexcept;
    ToolOutputSpill& operator=(ToolOutputSpill&&) = delete;
    ToolOutputSpill(const ToolOutputSpill&) = delete;
    ToolOutputSpill& operator=(const ToolOutputSpill&) = delete;
    ~ToolOutputSpill();

    // False once any write failed; later appends are no-ops.
    bool Append(std::string_view chunk);
    // Closes the file and prunes the session dir. Idempotent.
    void Finish();

    bool started() const { return started_; }
    bool ok() const { return started_ && error_.empty(); }
    const std::string& error() const { return error_; }
    // Project-relative path, as the model should pass it to read.
    std::string RelativePath() const;

    // The note appended after a cut result: where the full output is, or that
    // saving it failed. `shown` and `total` are byte counts.
    std::string Note(std::size_t shown, std::size_t total) const;

private:
    bool Open();

    std::filesystem::path project_root_;
    std::string session_id_;
    std::string name_;
    std::FILE* file_ = nullptr;
    bool started_ = false;
    bool finished_ = false;
    std::string error_;
};

// Cuts `text` to `cap` bytes (HeadTail), spilling the whole of it first, and
// appends the note. Within the cap: returned unchanged, nothing written.
std::string CapWithSpill(std::string text, std::size_t cap, ToolOutputSpill& spill);

}  // namespace minimal::tools
