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

// The per-project install mutex: two sessions ensuring packages concurrently
// must not race pip, and the "which versions do other sessions depend on"
// check must not race another session's install+spawn. Keyed on the project
// root string (all sessions live in one server process; an external terminal
// pip racing is the user's business, same as any shared .venv today).
// EnsureEnvironment/EnsurePackages do NOT lock: the caller holds this across
// the whole check -> install -> spawn -> register sequence.
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
    std::string system_python;          // settings.python (resolved on PATH at launch)
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

// The interpreter binary an invocation runs: the venv's python whenever the
// plan wants the venv (even before it exists -- the spawn creates it), else
// the system one resolved on PATH. Exists for disclosure: the Ask prompt
// names what will actually run.
std::string ResolveInterpreterBinary(const EnvPlan& plan);

// Creates the venv when missing (uv venv, else python -m venv) and installs
// plan.declared into it. Blocking, bounded by settings.install_timeout.
// Caller holds ProjectInstallMutex for plan.project_root. Empty error string
// on success; the return value describes what was done, for the tool result
// ("created .venv (uv); ensured impacket (uv pip)").
std::string EnsureEnvironment(const EnvPlan& plan, const PythonSettings& settings,
                              common::Logger& logger, std::string* error);

// Installs specs (package specs and "requirements:<path>" entries) into an
// EXISTING venv -- uv pip when uv is on PATH, else the venv's own pip.
// Caller holds ProjectInstallMutex for project_root.
std::string EnsurePackages(const std::filesystem::path& project_root,
                           const std::string& venv_python, const std::vector<std::string>& specs,
                           const PythonSettings& settings, common::Logger& logger,
                           std::string* error);

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
// "requirements:x" -> itself). PEP 503-normalized: lowercased, runs of
// -/_/. folded to one '-'.
std::string SpecName(const std::string& spec);

// Splits a declared/additive set into plain pip specs and requirements-file
// paths (the "requirements:<path>" entries MakeEnvPlan emits for a preset's
// `requirements:` list). Both EnsureEnvironment and EnsurePackages need this
// same split before building an install argv: a "requirements:x" entry
// handed to pip/uv as a literal spec fails as an invalid requirement.
// `requirement_paths` are relative, not yet resolved against a project root.
// Pure; no I/O.
// The cross-session half of the contract. The shared venv is used by every
// session in the project, so a declared entry whose name another LIVE
// session's interpreter already depends on at a different spec must not be
// installed (it would re-version that session's environment underneath it):
//   install   -- entries safe to install (new names, or identical specs);
//   record    -- the interpreter's declared record: `install` plus, for each
//                conflict, the spec the venv actually keeps;
//   conflicts -- formatted for the one-time advisory.
// Pure; no I/O.
struct SharedResolution {
    std::vector<std::string> install;
    std::vector<std::string> record;
    std::vector<std::string> conflicts;
};
SharedResolution ResolveAgainstShared(const std::vector<std::string>& declared,
                                      const std::vector<std::string>& in_use);

struct SplitSpecs {
    std::vector<std::string> package_specs;
    std::vector<std::string> requirement_paths;
};
SplitSpecs SplitDeclared(const std::vector<std::string>& entries);

}  // namespace minimal::tools_python
