#include "minimal/config/project_context.hpp"

#include <cstdint>
#include <fstream>
#include <system_error>
#include <utility>

namespace minimal::config {
namespace {

// Reads at most limit bytes (limit == 0 means the whole file) and reports
// whether anything was cut. Reading one byte past the bound is what makes
// "truncated" a fact rather than a second stat call racing the read.
bool ReadBounded(const std::filesystem::path& path, std::int64_t limit, std::string& out,
                 bool& truncated, std::uintmax_t& total_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    std::string buffer;
    if (limit > 0) {
        buffer.resize(static_cast<std::size_t>(limit) + 1);
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (in.bad()) return false;
        buffer.resize(static_cast<std::size_t>(in.gcount()));
    } else {
        std::string chunk(65536, '\0');
        while (true) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            if (in.bad()) return false;
            buffer.append(chunk.data(), static_cast<std::size_t>(in.gcount()));
            if (in.eof()) break;
        }
    }

    if (limit > 0 && static_cast<std::int64_t>(buffer.size()) > limit) {
        buffer.resize(static_cast<std::size_t>(limit));
        truncated = true;
    }
    std::error_code ec;
    total_bytes = std::filesystem::file_size(path, ec);
    if (ec) total_bytes = buffer.size();
    out = std::move(buffer);
    return true;
}

}  // namespace

std::optional<ProjectContext> LoadProjectContext(const std::filesystem::path& project_root,
                                                 const ProjectContextSettings& settings,
                                                 std::string* error) {
    if (error) error->clear();
    if (!settings.enabled || settings.files.empty()) return std::nullopt;

    // Walk from the real root: a symlinked project_root must not make the
    // ancestor search start somewhere its project_root does not actually sit.
    // Same canonicalize-first stance as path_confinement. Failure keeps the
    // path as given -- a root that cannot be resolved is still worth one
    // direct lookup.
    std::error_code ec;
    std::filesystem::path origin = std::filesystem::weakly_canonical(project_root, ec);
    if (ec) origin = project_root;

    // Nearest directory first, then the configured file order within it: a
    // CLAUDE.md in the project outranks an AGENTS.md three levels up, because
    // closer instructions are more specific. Without search_ancestors the
    // walk is one directory deep -- the documented default.
    for (std::filesystem::path dir = origin;
         !dir.empty();
         dir = settings.search_ancestors ? dir.parent_path() : std::filesystem::path()) {
        for (const std::string& name : settings.files) {
            const std::filesystem::path candidate = dir / name;
            ec.clear();
            if (!std::filesystem::is_regular_file(candidate, ec) || ec) continue;

            ProjectContext context;
            context.source_path = candidate;
            context.file_name = name;
            if (!ReadBounded(candidate, settings.max_bytes, context.body, context.truncated,
                             context.total_bytes)) {
                if (error != nullptr) {
                    *error = "project context file " + candidate.string() +
                             " could not be read (check permissions)";
                }
                return std::nullopt;
            }
            // An empty (or whitespace-only) file carries no instructions; skip
            // it and keep looking rather than rendering a section of nothing.
            if (context.body.find_first_not_of(" \t\r\n") == std::string::npos) continue;
            return context;
        }
        if (!settings.search_ancestors) break;
        // parent_path() is a fixed point at the filesystem root ("/" -> "/"),
        // so the path never empties; stop the walk when the parent stops
        // shrinking. Same idiom as project_dirs.cpp.
        if (dir == dir.parent_path()) break;
    }
    return std::nullopt;
}

std::string RenderProjectContextSection(const ProjectContext& context) {
    if (context.body.empty()) return {};
    std::string out;
    out.reserve(context.body.size() + 128);
    out += "## Project context\n\n";
    out += "Instructions from " + context.file_name + " in the project root:\n\n";
    out += context.body;
    if (context.truncated) {
        out += "\n\n[... " + context.file_name + " truncated: showing the first " +
               std::to_string(context.body.size()) + " of " +
               std::to_string(context.total_bytes) + " bytes]";
    }
    return out;
}

}  // namespace minimal::config
