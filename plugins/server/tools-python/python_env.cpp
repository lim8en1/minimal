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
// from ($PATH, $HOME & co -- bash_session_state.cpp): the interpreter (and the
// subprocesses executed code spawns) needs those and has no business
// inheriting provider API keys. The venv's bin directory is PREPENDED to PATH
// so pip and venv tools resolve; VIRTUAL_ENV is set for anything that sniffs
// it. PYTHONUNBUFFERED is deliberately absent: the child runs `python -I`,
// which implies -E and would ignore it anyway.
constexpr const char* kEnvAllowlist[] = {
    "PATH", "HOME", "USER", "LOGNAME", "SHELL", "TERM", "TERM_PROGRAM", "LANG", "TMPDIR",
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
    // Driver-side per-stream capture cap.
    env["_MINIMAL_PY_CAPTURE"] = std::to_string(settings.max_capture_bytes);
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
    if (plan.wants_venv) return plan.venv_python;
    const auto resolved = ResolveOnPath(plan.system_python);
    return resolved.empty() ? plan.system_python : resolved;
}

std::string EnsureEnvironment(const EnvPlan& plan, const PythonSettings& settings,
                              common::Logger& logger, std::string* error) {
    std::string note;
    *error = "";
    std::error_code ec;
    if (!std::filesystem::exists(plan.venv_python, ec)) {
        // CREATE. uv first (fast, no stdlib venv pitfalls), then the stdlib.
        std::string create_error;
        const auto helper_env = BuildChildEnv(plan, settings);
        if (const std::string uv = ResolveOnPath("uv"); !uv.empty()) {
            const auto run = RunCaptured({uv, "venv", plan.venv.string()}, plan.project_root,
                                         helper_env, settings.install_timeout, 64 * 1024);
            if (run.ok && run.exit_status == 0) {
                note = "created .venv (uv)";
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
                note = "created .venv (python -m venv)";
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
        const auto installed = EnsurePackages(plan.project_root, plan.venv_python, plan.declared,
                                              settings, logger, error);
        if (!error->empty()) return note;
        note += (note.empty() ? "" : "; ") + installed;
    }
    return note;
}

std::string EnsurePackages(const std::filesystem::path& project_root,
                           const std::string& venv_python, const std::vector<std::string>& specs,
                           const PythonSettings& settings, common::Logger& logger,
                           std::string* error) {
    *error = "";
    if (specs.empty()) return {};
    // "requirements:<path>" entries become -r arguments; handed to pip/uv as
    // literal specs they fail as invalid requirements.
    const auto split = SplitDeclared(specs);
    std::vector<std::string> argv;
    std::string via;
    if (const std::string uv = ResolveOnPath("uv"); !uv.empty()) {
        argv = {uv, "pip", "install", "--python", venv_python};
        via = "uv pip";
    } else {
        argv = {venv_python, "-m", "pip", "install"};
        via = "pip";
    }
    for (const auto& spec : split.package_specs) argv.push_back(spec);
    for (const auto& req : split.requirement_paths) {
        argv.push_back("-r");
        argv.push_back((project_root / req).string());
    }

    // The same child env as the interpreter: an install may spawn
    // subprocesses (source builds, VCS backends) and needs $HOME for caches.
    EnvPlan env_plan;
    env_plan.project_root = project_root;
    env_plan.venv = project_root / ".venv";
    env_plan.wants_venv = true;
    const auto run = RunCaptured(argv, project_root, BuildChildEnv(env_plan, settings),
                                 settings.install_timeout, 256 * 1024);
    if (!run.ok || run.exit_status != 0) {
        *error = "installing " + Join(specs, ", ") + " failed (" + via + "): " +
                 (run.error.empty() ? run.output : run.error + " " + run.output);
        return {};
    }
    logger.info("python: ensured [{}] in {}", Join(specs, ", "), venv_python);
    return "ensured " + Join(specs, ", ") + " (" + via + ")";
}

SplitSpecs SplitDeclared(const std::vector<std::string>& entries) {
    SplitSpecs out;
    for (const auto& entry : entries) {
        if (entry.rfind(kRequirementsPrefix, 0) == 0) {
            out.requirement_paths.push_back(entry.substr(std::strlen(kRequirementsPrefix)));
        } else {
            out.package_specs.push_back(entry);
        }
    }
    return out;
}

std::string SpecName(const std::string& spec) {
    if (spec.rfind(kRequirementsPrefix, 0) == 0) return spec;  // paths are their own identity
    // The name ends at the first version operator, extras bracket, marker,
    // direct-URL `@`, or whitespace; PEP 503 normalization then lowercases
    // and collapses every run of `-`, `_`, `.` into one `-`.
    std::string name;
    for (const char c : spec) {
        if (c == '\0' || std::strchr("=<>!~[;@(, \t", c) != nullptr) break;
        if (c == '-' || c == '_' || c == '.') {
            if (!name.empty() && name.back() != '-') name.push_back('-');
            continue;
        }
        name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
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

SharedResolution ResolveAgainstShared(const std::vector<std::string>& declared,
                                      const std::vector<std::string>& in_use) {
    SharedResolution out;
    std::unordered_map<std::string, std::string> used;  // name -> spec
    for (const auto& spec : in_use) used.emplace(SpecName(spec), spec);
    for (const auto& spec : declared) {
        const auto it = used.find(SpecName(spec));
        if (it != used.end() && it->second != spec) {
            out.conflicts.push_back(spec + " (another session in this project runs " +
                                    it->second + ")");
            out.record.push_back(it->second);  // what the shared venv actually keeps
            continue;
        }
        out.install.push_back(spec);
        out.record.push_back(spec);
    }
    return out;
}

}  // namespace minimal::tools_python
