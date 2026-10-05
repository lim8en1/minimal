#include "project_context.hpp"

#include "minimal/common/logger.hpp"

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace minimal::project_context {
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
    if (settings.files.empty()) return std::nullopt;

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
            // is_regular_file follows symlinks, and the body goes straight into
            // the system prompt (and to the provider) with no permission
            // prompt: a cloned repo's AGENTS.md -> ~/.aws/credentials must not
            // be read. The resolved file has to stay inside the directory it
            // was found in.
            {
                const auto resolved = std::filesystem::weakly_canonical(candidate, ec);
                if (ec) continue;
                const auto rel = resolved.lexically_relative(dir);
                if (rel.empty() || *rel.begin() == "..") continue;
            }

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

ProjectContextSettings ParseProjectContextSettings(const nlohmann::json& config) {
    ProjectContextSettings result;
    if (config.is_null()) return result;
    if (!config.is_object()) throw std::runtime_error("project-context: config must be an object");
    if (config.contains("search_ancestors")) {
        if (!config.at("search_ancestors").is_boolean()) {
            throw std::runtime_error("project-context: search_ancestors must be a boolean");
        }
        result.search_ancestors = config.at("search_ancestors").get<bool>();
    }
    if (config.contains("max_bytes")) {
        const auto& value = config.at("max_bytes");
        if (!value.is_number_integer() || value.get<std::int64_t>() < 0) {
            throw std::runtime_error("project-context: max_bytes must be a non-negative integer");
        }
        result.max_bytes = value.get<std::int64_t>();
    }
    if (config.contains("files")) {
        const auto& files = config.at("files");
        if (!files.is_array()) {
            throw std::runtime_error("project-context: files must be an array of file names");
        }
        result.files.clear();
        for (const auto& entry : files) {
            if (!entry.is_string() || entry.get<std::string>().empty()) {
                throw std::runtime_error("project-context: files must contain non-empty strings");
            }
            // File NAMES, not paths: the loader composes them against
            // project_root (and, with search_ancestors, its ancestors). A
            // separator would silently redefine the search, and an absolute
            // path would not follow the session at all.
            const std::string name = entry.get<std::string>();
            if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
                throw std::runtime_error("project-context: files entries must be plain file names, "
                                         "not paths ('" + name + "')");
            }
            result.files.push_back(name);
        }
    }
    return result;
}

namespace {

class ProjectContextContributor final : public minimal::agent::ContextContributor {
public:
    explicit ProjectContextContributor(ProjectContextSettings settings)
        : settings_(std::move(settings)) {}

    std::optional<minimal::agent::ContextSection> Contribute(
        const minimal::agent::ContextRequest& request) override {
        // Absence is the normal state and silent; a file that exists but
        // cannot be read is worth one warning -- the operator wrote
        // instructions and got silence.
        std::string error;
        auto context =
            LoadProjectContext(request.project_root, settings_, &error);
        if (!context) {
            if (!error.empty()) {
                minimal::common::DefaultLogger().warn("session {}: {}", request.session_id, error);
            }
            return std::nullopt;
        }
        minimal::common::DefaultLogger().info("session {}: loaded project context from {}",
                                              request.session_id, context->source_path.string());
        // Pre-rendered with its own heading and truncation marker.
        return minimal::agent::ContextSection{
            "", RenderProjectContextSection(*context)};
    }

    // Operator-authored framing of the project: ahead of everything else.
    int priority() const override { return 0; }
    // Sized by its own max_bytes (applied while loading, with a marker), not
    // the generic per-section cap.
    std::optional<std::int64_t> max_bytes() const override { return 0; }

private:
    ProjectContextSettings settings_;
};

}  // namespace

std::shared_ptr<agent::ContextContributor> MakeProjectContextContributor(
    ProjectContextSettings settings) {
    return std::make_shared<ProjectContextContributor>(std::move(settings));
}

}  // namespace minimal::project_context
