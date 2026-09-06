#include "python_env.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

// POSIX declares it in <unistd.h> only under feature-test macros that C++ mode
// does not guarantee; declare it ourselves (same as bash_session_state.cpp).
extern char** environ;

#include "minimal/common/logger.hpp"

#include "python_process.hpp"

namespace minimal::tools_python {
namespace {

// The server process's environment, filtered to the same allowlist bash seeds
// from: the interpreter (and the subprocesses executed code spawns) needs
// $HOME & co, and has no business inheriting provider API keys. The venv's
// bin directory is PREPENDED to PATH so pip and venv tools resolve; VIRTUAL_ENV
// is set for anything that sniffs it.
constexpr const char* kEnvAllowlist[] = {
    "HOME", "USER", "LOGNAME", "SHELL", "TERM", "TERM_PROGRAM", "LANG", "TMPDIR",
};
constexpr const char* kEnvAllowlistPrefixes[] = {"LC_"};

constexpr const char* kRequirementsPrefix = "requirements:";

std::string Join(const std::vector<std::string>& items, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += sep;
        out += items[i];
    }
    return out;
}

}  // namespace

std::map<std::string, std::string> BuildChildEnv(const EnvPlan& plan,
                                                 const PythonSettings& settings) {
    std::map<std::string, std::string> env;
    if (environ == nullptr) return env;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const char* equals = std::strchr(*entry, '=');
        if (equals == nullptr || equals == *entry) continue;  // malformed / empty key
        const std::string key(*entry, equals - *entry);
        const bool allowed =
            std::any_of(std::begin(kEnvAllowlist), std::end(kEnvAllowlist),
                        [&](const char* name) { return key == name; }) ||
            std::any_of(std::begin(kEnvAllowlistPrefixes), std::end(kEnvAllowlistPrefixes),
                        [&](const char* prefix) { return key.rfind(prefix, 0) == 0; });
        if (allowed) env.emplace(key, equals + 1);
    }
    // The venv first on PATH: pip and venv tools resolve there, and subprocesses
    // spawned by executed code find the project's tools.
    if (plan.wants_venv) {
        const auto bin = (plan.venv / "bin").string();
        const auto it = env.find("PATH");
        const std::string old_path = it == env.end() ? "/usr/local/bin:/usr/bin:/bin" : it->second;
        env["PATH"] = bin + ":" + old_path;
        env["VIRTUAL_ENV"] = plan.venv.string();
    }
    // Driver-side per-stream capture cap; unbuffered diagnostics.
    env["_MINIMAL_PY_CAPTURE"] = std::to_string(settings.max_capture_bytes);
    env["PYTHONUNBUFFERED"] = "1";
    return env;
}

ProjectInstallMutex& ProjectInstallMutex::Instance() {
    static ProjectInstallMutex instance;
    return instance;
}

std::unique_lock<std::mutex> ProjectInstallMutex::Lock(const std::filesystem::path& project_root) {
    std::lock_guard<std::mutex> lock(mu_);
    return std::unique_lock<std::mutex>(locks_[project_root.string()]);
}

EnvPlan MakeEnvPlan(const std::filesystem::path& project_root, const PythonSlice& slice,
                    const PythonSettings& settings) {
    EnvPlan plan;
    plan.project_root = project_root;
    plan.venv = project_root / ".venv";
    plan.venv_python = (plan.venv / "bin" / "python").string();
    plan.system_python = settings.python;
    for (const auto& spec : slice.packages) plan.declared.push_back(spec);
    for (const auto& path : slice.requirements) {
        plan.declared.push_back(std::string(kRequirementsPrefix) + path);
    }
    std::error_code ec;
    plan.wants_venv = !plan.declared.empty() || std::filesystem::exists(plan.venv, ec);
    return plan;
}

std::string ResolveInterpreterBinary(const EnvPlan& plan) {
    std::error_code ec;
    if (std::filesystem::exists(plan.venv_python, ec)) return plan.venv_python;
    return plan.system_python;
}

std::string ResolveUv(const PythonSettings& settings) {
    // Resolved per call, cheaply: `command -v`-style PATH walk, no pipe.
    const char* path_env = ::getenv("PATH");
    if (path_env == nullptr || *path_env == '\0') return {};
    std::error_code ec;
    std::string_view path(path_env);
    for (std::size_t start = 0; start <= path.size();) {
        const auto end = path.find(':', start);
        const auto dir = path.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                                          : end - start);
        if (!dir.empty()) {
            const auto candidate = std::filesystem::path(dir) / "uv";
            if (std::filesystem::is_regular_file(candidate, ec)) return candidate.string();
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    (void)settings;
    return {};
}

std::string EnsureEnvironment(const EnvPlan& plan, const PythonSettings& settings,
                              common::Logger& logger, std::string* error) {
    std::string note;
    const auto guard = ProjectInstallMutex::Instance().Lock(plan.project_root);
    (void)guard;

    std::error_code ec;
    const bool venv_ready = std::filesystem::exists(plan.venv_python, ec);
    if (!venv_ready && plan.declared.empty() && !plan.wants_venv) {
        *error = "";
        return note;  // bare system python; nothing to build
    }

    if (!venv_ready) {
        // CREATE. uv first (fast, no stdlib venv pitfalls), then the stdlib.
        std::string create_error;
        const auto helper_env = BuildChildEnv(plan, settings);
        const std::string uv = ResolveUv(settings);
        if (!uv.empty()) {
            const auto run = RunCaptured({uv, "venv", plan.venv.string()}, plan.project_root,
                                         helper_env, settings.install_timeout, 64 * 1024);
            if (run.ok && run.exit_status == 0) {
                note += "created .venv (uv)";
            } else {
                create_error = "uv venv: " +
                               (run.error.empty() ? run.output : run.error + " " + run.output);
            }
        } else {
            create_error = "uv not found";
        }
        if (!std::filesystem::exists(plan.venv_python, ec)) {
            const auto run = RunCaptured({settings.python, "-m", "venv", plan.venv.string()},
                                         plan.project_root, helper_env, settings.install_timeout,
                                         64 * 1024);
            if (run.ok && run.exit_status == 0) {
                note += note.empty() ? "created .venv (python -m venv)"
                                     : "; created .venv (python -m venv)";
            } else {
                create_error += "; python -m venv: " +
                                (run.error.empty() ? run.output : run.error + " " + run.output);
            }
        }
        if (!std::filesystem::exists(plan.venv_python, ec)) {
            *error = "could not create a virtual environment at " + plan.venv.string() + ": " +
                     create_error;
            return note;
        }
    }

    // INSTALL the declared contract, additively and idempotently. uv makes the
    // satisfied-check near-instant; the venv's own pip costs a few seconds.
    if (!plan.declared.empty()) {
        std::vector<std::string> specs;
        std::vector<std::string> requirement_paths;
        for (const auto& entry : plan.declared) {
            if (entry.rfind(kRequirementsPrefix, 0) == 0) {
                requirement_paths.push_back(entry.substr(std::strlen(kRequirementsPrefix)));
            } else {
                specs.push_back(entry);
            }
        }
        std::vector<std::string> argv;
        std::string via;
        if (const std::string uv = ResolveUv(settings); !uv.empty()) {
            argv = {uv, "pip", "install", "--python", plan.venv_python};
            via = "uv pip";
        } else {
            argv = {plan.venv_python, "-m", "pip", "install"};
            via = "pip";
        }
        for (const auto& spec : specs) argv.push_back(spec);
        for (const auto& req : requirement_paths) {
            argv.push_back("-r");
            argv.push_back((plan.project_root / req).string());
        }
        const auto run = RunCaptured(argv, plan.project_root, {}, settings.install_timeout,
                                     256 * 1024);
        if (!run.ok || run.exit_status != 0) {
            *error = "installing declared packages failed (" + via + "): " +
                     (run.error.empty() ? run.output : run.error + " " + run.output);
            return note;
        }
        note += note.empty() ? "" : "; ";
        note += "ensured declared packages (" + via + ")";
        logger.info("python: ensured declared set [{}] in {}", Join(plan.declared, ", "),
                    plan.venv.string());
    }
    *error = "";
    return note;
}

std::string EnsurePackages(const std::filesystem::path& project_root,
                           const std::string& venv_python, const std::vector<std::string>& specs,
                           const PythonSettings& settings, common::Logger& logger,
                           std::string* error) {
    const auto guard = ProjectInstallMutex::Instance().Lock(project_root);
    (void)guard;

    std::vector<std::string> argv;
    std::string via;
    if (const std::string uv = ResolveUv(settings); !uv.empty()) {
        argv = {uv, "pip", "install", "--python", venv_python};
        via = "uv pip";
    } else {
        argv = {venv_python, "-m", "pip", "install"};
        via = "pip";
    }
    for (const auto& spec : specs) argv.push_back(spec);

    EnvPlan plan_like;
    plan_like.project_root = project_root;
    plan_like.venv = project_root / ".venv";
    plan_like.wants_venv = true;
    const auto run = RunCaptured(argv, project_root, BuildChildEnv(plan_like, settings),
                                 settings.install_timeout, 256 * 1024);
    if (!run.ok || run.exit_status != 0) {
        *error = "installing " + Join(specs, ", ") + " failed (" + via + "): " +
                 (run.error.empty() ? run.output : run.error + " " + run.output);
        return {};
    }
    logger.info("python: auto-ensured [{}] in {}", Join(specs, ", "), venv_python);
    *error = "";
    return "installed: " + Join(specs, ", ");
}

std::string SpecName(const std::string& spec) {
    if (spec.rfind(kRequirementsPrefix, 0) == 0) return spec;  // paths are their own identity
    std::string name;
    for (const char c : spec) {
        if (c == '=' || c == '<' || c == '>' || c == '!' || c == '~' || c == '[' || c == ' ') break;
        name.push_back(c == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    while (!name.empty() && name.back() == '-') name.pop_back();
    return name;
}

DeclaredDiff ClassifyDiff(const std::vector<std::string>& current,
                          const std::vector<std::string>& interpreter_record) {
    DeclaredDiff diff;
    std::unordered_map<std::string, std::string> have;  // name -> spec
    for (const auto& spec : interpreter_record) have[SpecName(spec)] = spec;
    for (const auto& spec : current) {
        const auto name = SpecName(spec);
        const auto it = have.find(name);
        if (it == have.end()) {
            diff.additive.push_back(spec);
        } else if (it->second != spec) {
            diff.conflicts.push_back(spec + " (running session has " + it->second + ")");
        }
    }
    return diff;
}

}  // namespace minimal::tools_python
