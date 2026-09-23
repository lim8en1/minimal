#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "minimal/common/logger.hpp"

#include "python_settings.hpp"

namespace minimal::tools_python {

// THE ENVIRONMENT MODEL: state is per-session (one interpreter process per
// session), environment is per-project (ONE shared venv at
// <project_root>/.venv, used by every agent, tab, and subagent in that
// project). No per-preset venv override in v1 -- version conflicts between
// presets are made LOUD (see ClassifyDiff) rather than isolated; an override
// is a purely additive v2 escape hatch.

// The per-project install mutex: two sessions auto-ensuring concurrently must
// not race pip. Keyed on the project root string (all sessions live in one
// server process; an external terminal pip racing is the user's business,
// same as any shared .venv today).
class ProjectInstallMutex {
public:
    static ProjectInstallMutex& Instance();
    std::unique_lock<std::mutex> Lock(const std::filesystem::path& project_root);

private:
    std::mutex mu_;
    std::map<std::string, std::mutex> locks_;
};

// What the current resolution says this session's python environment is.
struct EnvPlan {
    std::filesystem::path project_root;
    std::filesystem::path venv;         // <project_root>/.venv (always, v1)
    std::string venv_python;            // <venv>/bin/python (may not exist yet)
    std::string system_python;          // settings.python resolved on PATH
    // The DECLARED contract: package specs plus "requirements:<path>" entries.
    std::vector<std::string> declared;
    // True when the session wants the venv: anything declared, or the venv
    // already exists. False means "no declared contract, no venv" -> bare
    // system python.
    bool wants_venv = false;
};

// Pure (no I/O): assembles the plan from the preset slice + settings.
EnvPlan MakeEnvPlan(const std::filesystem::path& project_root, const PythonSlice& slice,
                    const PythonSettings& settings);

// The resolved interpreter binary (the venv's python when it exists, else the
// system one). Exists for disclosure: the Ask prompt names the resolved path.
std::string ResolveInterpreterBinary(const EnvPlan& plan);

// Creates the venv when missing (uv venv, else python -m venv) and installs
// the declared packages/requirements into it. Blocking, bounded by
// settings.install_timeout; serialized per project. Empty error string on
// success. `note` describes what was done, for the tool result ("created
// .venv (uv); installed: impacket").
std::string EnsureEnvironment(const EnvPlan& plan, const PythonSettings& settings,
                              common::Logger& logger, std::string* error);

// Installs additional specs into an EXISTING venv (the auto-ensure path on a
// preset switch). Serialized per project like EnsureEnvironment.
std::string EnsurePackages(const std::filesystem::path& project_root,
                           const std::string& venv_python, const std::vector<std::string>& specs,
                           const PythonSettings& settings, common::Logger& logger,
                           std::string* error);

// Where uv lives, resolved once per process (empty = not installed).
std::string ResolveUv(const PythonSettings& settings);

// The child interpreter's full environment: the bash env allowlist (never the
// server process's credentials), the venv's bin prepended to PATH,
// VIRTUAL_ENV, and the driver's capture-cap knob. Also used for the helper
// commands (venv creation, installs).
std::map<std::string, std::string> BuildChildEnv(const EnvPlan& plan,
                                                 const PythonSettings& settings);

// --- The declared-set diff (the per-exec comparison) ---------------------------

struct DeclaredDiff {
    // Entries the active preset declares that the interpreter's contract does
    // not: a NEW package name (or requirements path). Safe to auto-ensure --
    // additive, and packages installed into the shared venv are immediately
    // importable by the running interpreter.
    std::vector<std::string> additive;
    // Same package NAME with a DIFFERENT spec ("foo==2" vs the running
    // session's "foo==1"). Installing would silently re-version the shared
    // environment under every other agent -- reported loudly instead, once
    // per interpreter, and left to the model's explicit choice.
    std::vector<std::string> conflicts;  // formatted "foo==2 (running session has foo==1)"
};

DeclaredDiff ClassifyDiff(const std::vector<std::string>& current,
                          const std::vector<std::string>& interpreter_record);

// The distribution name of a spec ("pefile==2023.2.7" -> "pefile",
// "requirements:x" -> itself). Lowercased; -/_ folded, as pip treats them.
std::string SpecName(const std::string& spec);

// Splits a declared/additive set into plain pip specs and requirements-file
// paths (the "requirements:<path>" entries MakeEnvPlan emits for a preset's
// `requirements:` list). Both EnsureEnvironment and EnsurePackages need this
// same split before building an install argv: a "requirements:x" entry
// handed to pip/uv as a literal spec fails as an invalid requirement.
// `requirement_paths` are relative, not yet resolved against a project root.
// Pure; no I/O.
struct SplitSpecs {
    std::vector<std::string> package_specs;
    std::vector<std::string> requirement_paths;
};
SplitSpecs SplitDeclared(const std::vector<std::string>& entries);

}  // namespace minimal::tools_python
