#include "minimal/tools/tool_output_spill.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

#include "minimal/common/atomic_write.hpp"
#include "minimal/common/utf8.hpp"
#include "minimal/tools/policy.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::tools {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kSpillRoot = ".minimal/tool-output";

// A name for a spill whose call has no tool_use_id (tests, internal calls):
// unique within the process and sortable by creation.
std::string UniqueName() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return "out-" + std::to_string(now) + "-" + std::to_string(counter.fetch_add(1)) + ".txt";
}

// Only a plain file name: a tool_use_id is backend-issued, and a crafted one
// must not walk out of the session dir.
std::string SafeName(std::string name) {
    if (name.empty()) return UniqueName();
    for (char& c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!plain) c = '_';
    }
    if (name.front() == '.') name.front() = '_';
    for (auto dots = name.find(".."); dots != std::string::npos; dots = name.find("..", dots)) {
        name[dots] = '_';
    }
    return name;
}

// Keeps the newest kSpillKeepPerSession regular files. Best effort: a racing
// prune or an unreadable entry costs nothing but a stale file.
void Prune(const fs::path& dir) {
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec)) continue;
        files.emplace_back(it->last_write_time(entry_ec), it->path());
    }
    if (files.size() <= kSpillKeepPerSession) return;
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = kSpillKeepPerSession; i < files.size(); ++i) {
        fs::remove(files[i].second, ec);
    }
}

}  // namespace

fs::path ToolOutputDir(const fs::path& project_root, const std::string& session_id) {
    fs::path dir = project_root / kSpillRoot;
    if (!session_id.empty()) dir /= SafeName(session_id);
    return dir;
}

std::string HeadTail(std::string_view text, std::size_t cap) {
    if (text.size() <= cap) return std::string(text);
    const std::size_t head_end = common::Utf8FloorToBoundary(text, cap / 4);
    const std::size_t tail_start = common::Utf8CeilToBoundary(text, text.size() - (cap - cap / 4));
    const std::string marker =
        "\n[... " + std::to_string(tail_start - head_end) + " bytes omitted ...]\n";
    std::string out;
    out.reserve(head_end + marker.size() + (text.size() - tail_start));
    out.append(text.substr(0, head_end));
    out += marker;
    out.append(text.substr(tail_start));
    return out;
}

ToolOutputSpill::ToolOutputSpill(fs::path project_root, std::string session_id, std::string name)
    : project_root_(std::move(project_root)),
      session_id_(std::move(session_id)),
      name_(SafeName(std::move(name))) {}

ToolOutputSpill ToolOutputSpill::For(const ToolContext& ctx) {
    return ToolOutputSpill(ctx.project_root, ctx.session_id,
                           ctx.tool_use_id.empty() ? std::string() : ctx.tool_use_id + ".txt");
}

ToolOutputSpill::ToolOutputSpill(ToolOutputSpill&& other) noexcept
    : project_root_(std::move(other.project_root_)),
      session_id_(std::move(other.session_id_)),
      name_(std::move(other.name_)),
      file_(std::exchange(other.file_, nullptr)),
      started_(other.started_),
      finished_(std::exchange(other.finished_, true)),
      error_(std::move(other.error_)) {}

ToolOutputSpill::~ToolOutputSpill() { Finish(); }

bool ToolOutputSpill::Open() {
    started_ = true;
    if (project_root_.empty()) {
        error_ = "no project root to save it under";
        return false;
    }
    const fs::path root = project_root_ / kSpillRoot;
    const fs::path dir = ToolOutputDir(project_root_, session_id_);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        error_ = "cannot create " + dir.string() + ": " + ec.message();
        return false;
    }
    // Scoped to tool-output/ alone: spills never show in git status, and a
    // project that tracks other .minimal/ content keeps tracking it.
    const fs::path ignore = root / ".gitignore";
    if (!fs::exists(ignore, ec)) (void)common::write_file(ignore, "*\n");

    // 0600: tool output can carry whatever the command printed, secrets
    // included; the spill is for this user's model, not the group.
    const fs::path path = dir / name_;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        error_ = "cannot open " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    file_ = ::fdopen(fd, "wb");
    if (file_ == nullptr) {
        error_ = std::string("fdopen: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    return true;
}

bool ToolOutputSpill::Append(std::string_view chunk) {
    if (finished_) return false;
    if (!started_ && !Open()) return false;
    if (!error_.empty()) return false;
    if (chunk.empty()) return true;
    if (std::fwrite(chunk.data(), 1, chunk.size(), file_) != chunk.size()) {
        error_ = std::string("write failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

void ToolOutputSpill::Finish() {
    if (finished_) return;
    finished_ = true;
    if (file_ != nullptr) {
        if (std::fclose(file_) != 0 && error_.empty()) {
            error_ = std::string("close failed: ") + std::strerror(errno);
        }
        file_ = nullptr;
    }
    if (started_) Prune(ToolOutputDir(project_root_, session_id_));
}

bool ToolOutputSpill::AdoptExisting() {
    if (started_) return false;
    std::error_code ec;
    if (!fs::is_regular_file(ToolOutputDir(project_root_, session_id_) / name_, ec)) return false;
    started_ = true;
    finished_ = true;
    return true;
}

std::string ToolOutputSpill::RelativePath() const {
    return (ToolOutputDir(fs::path(), session_id_) / name_).generic_string();
}

std::string ToolOutputSpill::Note(std::size_t shown, std::size_t total) const {
    std::string note = "[Output cut to " + std::to_string(shown) + " of " +
                       std::to_string(total) + " bytes (the start and the end). ";
    if (ok()) {
        note += "The full output is saved at " + RelativePath() +
                " -- use read (offset/limit) or grep on it rather than re-running.]";
    } else {
        note += "Saving the full output failed (" + (error_.empty() ? "not written" : error_) +
                ") -- re-run with narrower output (head, tail, grep) if you need more.]";
    }
    return note;
}

std::string CapWithSpill(std::string text, std::size_t cap, ToolOutputSpill& spill) {
    if (text.size() <= cap) return text;
    const std::size_t total = text.size();
    spill.Append(text);
    spill.Finish();
    return HeadTail(text, cap) + "\n" + spill.Note(cap, total);
}

}  // namespace minimal::tools
