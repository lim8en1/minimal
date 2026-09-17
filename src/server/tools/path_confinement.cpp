#include "minimal/tools/path_confinement.hpp"

namespace minimal::tools {

ConfineResult confine(const std::filesystem::path& project_root,
                       const std::string& user_supplied,
                       bool must_exist) {
    ConfineResult result;

    if (user_supplied.find('\0') != std::string::npos) {
        result.error = "path contains an embedded NUL";
        return result;
    }

    std::error_code ec;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(project_root, ec);
    if (ec) {
        result.error = "cannot resolve project_root: " + ec.message();
        return result;
    }

    const std::filesystem::path candidate = project_root / user_supplied;
    std::filesystem::path resolved;
    if (must_exist) {
        resolved = std::filesystem::canonical(candidate, ec);
    } else {
        resolved = std::filesystem::weakly_canonical(candidate, ec);
    }
    if (ec) {
        // Covers symlink loops (ELOOP) and any other path the filesystem itself
        // cannot canonicalize.
        result.error = "cannot resolve path: " + ec.message();
        return result;
    }

    result.absolute_path = resolved;
    result.ok = true;

    const std::filesystem::path relative = resolved.lexically_relative(canonical_root);
    result.outside_project_root = relative.empty() || relative.begin()->string() == "..";
    result.relative_to_root = relative;
    return result;
}

std::string ProjectRelativePrefix(const ConfineResult& resolved, bool is_dir) {
    if (!resolved.ok) return "";
    const auto& rel = resolved.relative_to_root;
    if (rel.empty() || rel.is_absolute()) return "";
    if (rel == ".") return "";  // the root itself: bare names are already correct
    // An outside-root result lexically relatives to a ".."-prefixed path;
    // prefixing with it would point every emitted path out of the project.
    if (rel.begin()->string() == "..") return "";
    if (is_dir) return rel.generic_string() + "/";
    // A file: the walker reports it under its bare filename, so the prefix is
    // the PARENT's project-relative directory ("sub/" for sub/solo.txt), not
    // the file's own path.
    const auto parent = rel.parent_path();
    if (parent.empty() || parent == ".") return "";
    return parent.generic_string() + "/";
}

}  // namespace minimal::tools
