#include "minimal/config/sanity_check.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_set>

#include "minimal/common/config_paths.hpp"

namespace minimal::config {
namespace {

CheckResult Fatal(const std::string& check, const std::string& message) {
    return {CheckSeverity::Fatal, check, message};
}

CheckResult Warning(const std::string& check, const std::string& message) {
    return {CheckSeverity::Warning, check, message};
}

std::string ReadFileOrEmpty(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string JoinNames(const std::vector<std::string>& names) {
    if (names.empty()) return "nothing";
    if (names.size() == 1) return names.front();
    std::string out = names.front();
    for (std::size_t i = 1; i + 1 < names.size(); ++i) out += ", " + names[i];
    return out + " and " + names.back();
}

// Resolvable the way a spawned process would find it: a bare name is searched
// against PATH, anything containing a slash is used as-is. POSIX access(2)
// rather than std::filesystem::access (C++20, absent from this libc++).
bool CommandResolves(const std::string& command) {
    if (command.empty()) return false;
    const auto executable = [](const std::filesystem::path& path) {
        std::error_code ec;
        return !ec && std::filesystem::is_regular_file(path, ec) && ::access(path.c_str(), X_OK) == 0;
    };
    if (command.find('/') != std::string::npos) {
        return executable(command);
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;
    std::stringstream streams(path_env);
    std::string dir;
    while (std::getline(streams, dir, ':')) {
        if (dir.empty()) continue;
        if (executable(std::filesystem::path(dir) / command)) return true;
    }
    return false;
}

bool UrlLooksWellFormed(const std::string& url) {
    const auto sep = url.find("://");
    if (sep == std::string::npos || sep == 0) return false;
    const auto scheme = url.substr(0, sep);
    if (scheme != "http" && scheme != "https") return false;
    const auto rest = url.substr(sep + 3);
    const auto host_end = rest.find_first_of("/?#");
    return !rest.substr(0, host_end).empty();
}

}  // namespace

StartupSanityCheck::StartupSanityCheck(SanityCheckInputs inputs) : inputs_(std::move(inputs)) {}

std::vector<CheckResult> StartupSanityCheck::RunAll() {
    std::vector<CheckResult> results;
    // Order matters only for readability: EVERY check runs regardless of what
    // the earlier ones found -- that is the linter property. Config files go
    // first simply because the later checks read whatever they managed to load.
    auto append = [&results](std::vector<CheckResult> more) {
        results.insert(results.end(), std::make_move_iterator(more.begin()),
                       std::make_move_iterator(more.end()));
    };
    append(check_config_files());
    append(check_providers());
    append(check_presets());
    append(check_mcp_servers());
    append(check_directories());
    append(check_session_db());
    append(check_plugins());
    append(check_name_collisions());
    append(check_naming_model());
    append(check_advisor());
    append(check_subagent_limit());
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_config_files() {
    std::vector<CheckResult> results;
    const auto& dir = inputs_.config_dir;

    try {
        settings_ = LoadSettingsConfig(dir / "settings.json");
        settings_loaded_ = true;
    } catch (const std::exception& e) {
        // Serving requires this file: the preset registry and the session
        // service both consume it, so absence is fatal, not degraded.
        results.push_back(Fatal("config_parse", std::string("settings.json: ") + e.what()));
    }

    try {
        permissions_ = LoadPermissionsConfig(dir / "permissions.json");
    } catch (const std::exception& e) {
        results.push_back(Fatal("config_parse", std::string("permissions.json: ") + e.what()));
    }

    try {
        providers_ = LoadProvidersConfig(dir / "providers.json");
        providers_loaded_ = true;
    } catch (const std::exception& e) {
        results.push_back(Fatal("config_parse", std::string("providers.json: ") + e.what()));
    }

    // plugins.json degrades to empty BY DESIGN -- but a silently degraded file
    // is exactly what this check exists to make loud.
    const auto plugins_text = ReadFileOrEmpty(dir / "plugins.json");
    if (!plugins_text.empty()) {
        const auto probe = nlohmann::json::parse(plugins_text, nullptr, /*allow_exceptions=*/false);
        if (probe.is_discarded() || !probe.is_object()) {
            results.push_back(Warning("config_parse", "plugins.json is malformed; treating it as empty "
                                                       "(discovered plugins keep their provenance defaults)"));
        }
    }

    // A broken mcps.json degrades to "no MCP servers" plus a warning: MCP is an
    // optional feature the core serving path does not depend on.
    try {
        mcps_ = LoadMcpsConfig(dir / "mcps.json");
    } catch (const std::exception& e) {
        results.push_back(Warning("config_parse", std::string("mcps.json: ") + e.what() +
                                                    "; no MCP servers will be configured"));
    }
    for (const auto& error : mcps_.errors) {
        results.push_back(Warning("mcp_servers", error));
    }

    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_providers() {
    std::vector<CheckResult> results;
    if (!providers_loaded_) return results;  // already Fatal under config_parse

    // A provider is a CONNECTION and the addressable unit is <provider>/<model>,
    // so this walks models rather than entries: a provider whose backend no
    // plugin registered makes every model under it unusable, and saying so once
    // per model is what tells the user which references are dead.
    const auto kinds = inputs_.provider_factories.kinds();
    const auto refs = providers_.ModelRefs();
    bool any_usable = false;
    std::unordered_map<std::string, bool> backend_reported;

    for (const auto& ref_text : refs) {
        const auto ref = common::ParseModelRef(ref_text);
        if (!ref) continue;  // ParseProvidersConfig rejects these already
        const ResolvedModel* model = providers_.Resolve(*ref);
        if (model == nullptr) continue;

        const bool registered =
            std::find(kinds.begin(), kinds.end(), model->backend) != kinds.end();
        if (!registered) {
            // Once per provider, not once per model: five models behind one dead
            // backend is one problem, and five copies of it buries the others.
            if (!backend_reported[ref->provider]) {
                backend_reported[ref->provider] = true;
                results.push_back(Warning("usable_provider",
                                          "providers.json provider '" + ref->provider +
                                              "': backend '" + model->backend +
                                              "' is not registered by any loaded plugin"));
            }
            continue;
        }
        any_usable = true;
    }

    if (!any_usable) {
        // A harness that cannot talk to an LLM is useless, not degraded -- the
        // same standard as a failed bundled plugin.
        const std::string available = kinds.empty() ? "none" : JoinNames(kinds);
        results.push_back(Fatal(
            "usable_provider",
            refs.empty()
                ? "no usable model: providers.json defines none at all -- add a provider with a "
                  "nested \"models\" object, and a backend a loaded plugin registers (available "
                  "kinds: " +
                      available + "). See providers.example.json."
                : "no usable model: none of the configured providers uses a registered backend "
                  "(available kinds: " +
                      available + ")"));
    }

    // The default has to name something, or every session that does not override
    // it fails at creation with the same message.
    if (settings_loaded_ && !settings_.default_model.empty()) {
        const auto ref = common::ParseModelRef(settings_.default_model);
        if (!ref) {
            results.push_back(Warning("usable_provider",
                                      "settings.json default_model '" + settings_.default_model +
                                          "' is not <provider>/<model>"));
        } else if (providers_.Resolve(*ref) == nullptr) {
            results.push_back(Warning("usable_provider",
                                      "settings.json default_model '" + settings_.default_model +
                                          "' matches no configured model" +
                                          (refs.empty() ? "" : " (have: " + JoinNames(refs) + ")")));
        }
    }

    // A kind two plugins both register: the first wins silently -- make it loud.
    std::unordered_map<std::string, std::string> kind_owner;
    for (const auto& record : inputs_.plugin_records) {
        if (record.status != plugins::LoadStatus::Loaded) continue;
        const auto label = record.name.empty() ? record.file.filename().string() : record.name;
        for (const auto& kind : record.registered_provider_kinds) {
            const auto [it, inserted] = kind_owner.try_emplace(kind, label);
            if (!inserted) {
                results.push_back(Warning("provider_kinds", "provider kind '" + kind + "' is registered by both " +
                                     it->second + " and " + label + "; the first wins"));
            }
        }
    }

    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_presets() {
    std::vector<CheckResult> results;
    const auto agents_dir = inputs_.config_dir / "agents";

    // Parse every preset EAGERLY: AgentPresetRegistry loads them lazily, which
    // is exactly why a broken one stayed silent until the first session.
    std::error_code ec;
    if (std::filesystem::is_directory(agents_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(agents_dir, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".md") continue;
            const auto preset_name = entry.path().stem().string();
            // subagent.md is plain Markdown without frontmatter -- a session-level
            // prefix, not a preset (ARCHITECTURE.md, "Config").
            if (preset_name == "subagent") continue;
            try {
                presets_[preset_name] = agent::LoadAgentPresetFrontmatter(entry.path());
            } catch (const std::exception& e) {
                results.push_back(Warning("presets", "agents/" + preset_name + ".md: " + e.what()));
            }
        }
    }

    if (presets_.empty()) {
        // No session can be created at all -- the cold-start case. Loud by
        // design: there is no onboarding wizard, just a check with a reason.
        results.push_back(Fatal("presets", "no agent preset available -- create agents/<name>.md "
                             "(frontmatter: name, model: <provider>/<model>; body: system prompt) and set "
                             "settings.json default_agent_preset"));
    }

    // Resolves the preset's model the way AgentPresetRegistry would: the
    // preset's own <provider>/<model>, or settings.default_model when it
    // declares none.
    const auto preset_problem = [this](const agent::AgentPresetFrontmatter& raw) -> std::optional<std::string> {
        if (!providers_loaded_) return std::nullopt;  // config_parse already names it
        std::string model_text = raw.model;
        if (model_text.empty()) {
            model_text = settings_loaded_ ? settings_.default_model : "";
            if (model_text.empty()) {
                return "declares no model and settings.json sets no default_model";
            }
        }
        const auto ref = common::ParseModelRef(model_text);
        if (!ref) return "model '" + model_text + "' is not <provider>/<model>";
        const ResolvedModel* model = providers_.Resolve(*ref);
        if (model == nullptr) return "names unknown model '" + model_text + "'";
        const auto kinds = inputs_.provider_factories.kinds();
        if (std::find(kinds.begin(), kinds.end(), model->backend) == kinds.end()) {
            return "model '" + model_text + "' needs backend '" + model->backend +
                   "', which no loaded plugin registers";
        }
        return std::nullopt;
    };

    // The DEFAULT preset must resolve end to end: every client that does not
    // name a preset (the headless client's common case) would fail otherwise.
    if (settings_loaded_ && !settings_.default_agent_preset.empty()) {
        const auto it = presets_.find(settings_.default_agent_preset);
        if (it == presets_.end()) {
            results.push_back(Fatal("presets", "default_agent_preset '" + settings_.default_agent_preset +
                                 "' does not resolve: agents/" + settings_.default_agent_preset +
                                 ".md is missing or failed to parse"));
        } else if (auto problem = preset_problem(it->second)) {
            results.push_back(Fatal("presets", "default_agent_preset '" + settings_.default_agent_preset +
                                 "' does not resolve: " + *problem));
        }
    }

    for (const auto& [preset_name, raw] : presets_) {
        const bool is_default = settings_loaded_ && preset_name == settings_.default_agent_preset;
        // Non-default presets break only the sessions that pick them; a broken
        // provider chain on the DEFAULT preset was just reported as Fatal above,
        // and repeating it as a Warning would read as two problems instead of
        // one. The compression-prompt check still applies to it: a default
        // preset's missing custom prompt is a distinct problem.
        if (!is_default) {
            if (auto problem = preset_problem(raw)) {
                results.push_back(Warning("presets", "agent preset '" + preset_name + "': " + *problem));
            }
        }
        if (!raw.compression_prompt.empty() &&
            !std::filesystem::exists(inputs_.config_dir / "prompts" / (raw.compression_prompt + ".md"), ec)) {
            results.push_back(Warning("presets", "agent preset '" + preset_name +
                                 "': compression prompt 'prompts/" + raw.compression_prompt + ".md' not found"));
        }
    }

    // The default compaction prompt: absence degrades every session's compaction
    // to a log line, and making that loud is this check's job (ARCHITECTURE.md,
    // "Bundled assets").
    if (!std::filesystem::exists(inputs_.config_dir / "prompts" / "compression.md", ec)) {
        results.push_back(Warning("presets", "prompts/compression.md not found -- compaction is disabled "
                             "for every preset that does not override it"));
    }

    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_mcp_servers() {
    std::vector<CheckResult> results;
    for (const auto& entry : mcps_.entries) {
        if (!entry.enabled) continue;  // a disabled entry is not going to spawn
        if (entry.transport == McpTransport::Stdio) {
            if (!CommandResolves(entry.command)) {
                results.push_back(Warning("mcp_servers", "entry '" + entry.name + "': command '" + entry.command +
                                     "' does not resolve on PATH"));
            }
        } else if (!UrlLooksWellFormed(entry.url)) {
            results.push_back(Warning("mcp_servers", "entry '" + entry.name + "': url '" + entry.url +
                                 "' is not a well-formed http(s) url"));
        }
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_directories() {
    std::vector<CheckResult> results;
    auto check_dir = [](const std::filesystem::path& path, const char* what) -> std::optional<std::string> {
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec) || ec) {
            return std::string(what) + " '" + path.string() + "' does not exist or is not a directory";
        }
        if (::access(path.c_str(), W_OK) != 0) {
            return std::string(what) + " '" + path.string() + "' is not writable";
        }
        return std::nullopt;
    };

    // The server's own footprint. project_root itself is per-session (the client
    // supplies its cwd in CreateSessionRequest), so it is validated there, not
    // here -- see SessionServiceImpl::CreateSession.
    if (auto problem = check_dir(inputs_.config_dir, "config dir")) {
        results.push_back(Fatal("directories", *problem));
    }
    if (auto problem = check_dir(common::DataDir(), "data dir")) {
        results.push_back(Fatal("directories", *problem + " -- the session database and logs live there"));
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_session_db() {
    std::vector<CheckResult> results;
    // If settings failed to load, fall back to the default path and still run
    // the check -- the linter property. The config_parse Fatal already names
    // the real problem; this one reports whether the database itself is sound.
    const auto db_path =
        session::ResolveSessionDbPath(settings_loaded_ ? settings_.session_db_path : std::string{});
    try {
        // Opening migrates, and refuses a database this binary is too old to
        // understand -- fatal by design, since running on would silently drop
        // every transcript.
        session_db_ = std::make_unique<session::SessionDatabase>(db_path);
    } catch (const std::exception& e) {
        results.push_back(Fatal("session_db", "session database '" + db_path.string() + "': " + e.what()));
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_plugins() {
    std::vector<CheckResult> results;
    for (const auto& record : inputs_.plugin_records) {
        // Loaded is fine. Disabled is a DELIBERATE choice -- the documented
        // tool-replacement flow -- and its practical impact (no provider kind,
        // no preset tools) is caught by usable_provider/presets instead.
        if (record.status == plugins::LoadStatus::Loaded || record.status == plugins::LoadStatus::Disabled) {
            continue;
        }
        const auto label = record.name.empty() ? record.file.filename().string() : record.name;
        const std::string provenance =
            record.provenance == plugins::Provenance::Bundled ? "bundled" : "user";
        const std::string message = "plugin '" + label + "' (" + provenance + "): " +
                                    std::string(plugins::ToString(record.status)) +
                                    (record.error.empty() ? "" : ": " + record.error);
        // Severity splits by provenance: losing tools-fs or provider-openai is a
        // useless harness, not a degraded one; a user plugin is swappable.
        if (record.provenance == plugins::Provenance::Bundled) {
            results.push_back(Fatal("plugins", message));
        } else {
            results.push_back(Warning("plugins", message));
        }
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_name_collisions() {
    std::vector<CheckResult> results;
    const auto& resolution = inputs_.name_resolution;

    // Who claims a surface name, across ALL registries. NameRegistry reports the
    // colliding name; the attributions supply the claimants, which is what makes
    // the diagnostic actionable -- the name alone does not say which plugin to
    // disable. Tools, commands and services are separate namespaces, so the kind
    // is part of the answer rather than an aside.
    enum class ClaimKind { kTool, kCommand, kService };
    const auto claimants = [this](const std::string& surface, ClaimKind& kind) {
        std::vector<std::string> owners;
        const auto add = [&owners](const std::string& plugin_name) {
            const auto label = plugin_name.empty() ? "core" : plugin_name;
            if (std::find(owners.begin(), owners.end(), label) == owners.end()) {
                owners.push_back(label);
            }
        };

        for (const auto& attr : inputs_.tool_registry.attributions()) {
            std::string s = attr.tool_name;
            const auto qualified = attr.plugin_name + "." + attr.tool_name;
            if (auto it = inputs_.plugin_config.command_aliases.find(qualified);
                it != inputs_.plugin_config.command_aliases.end()) {
                s = it->second;
            }
            if (s == surface) add(attr.plugin_name);
        }
        if (!owners.empty()) {
            kind = ClaimKind::kTool;
            return owners;
        }

        // A command collision, then. Its registry has already applied aliases, so
        // surface_name is what to compare -- no second alias lookup here.
        if (inputs_.command_registry != nullptr) {
            for (const auto& attr : inputs_.command_registry->attributions()) {
                if (attr.surface_name == surface) add(attr.plugin_name);
            }
            if (!owners.empty()) kind = ClaimKind::kCommand;
        }
        if (!owners.empty()) return owners;

        // A service collision, then. Original names register unaliased, so the
        // attribution's name is what to compare.
        if (inputs_.service_registry != nullptr) {
            for (const auto& attr : inputs_.service_registry->attributions()) {
                if (attr.service_name == surface) add(attr.plugin_name);
            }
            if (!owners.empty()) kind = ClaimKind::kService;
        }
        return owners;
    };

    for (const auto& surface : resolution.result.fatal_collisions) {
        ClaimKind kind = ClaimKind::kTool;
        const auto owners = claimants(surface, kind);
        if (kind == ClaimKind::kService) {
            results.push_back(Fatal("name_collisions",
                                    "service name '" + surface + "' is provided by " +
                                        JoinNames(owners) + " -- disable one of the plugins"));
            continue;
        }
        results.push_back(
            Fatal("name_collisions",
                  std::string(kind == ClaimKind::kCommand ? "command name '/" : "tool name '") +
                      surface +
                      "' is provided by " + JoinNames(owners) +
                      " -- disable one plugin, or give one of them a command_aliases entry"));
    }

    for (const auto& key : resolution.result.dangling_aliases) {
        results.push_back(Warning("aliases", "command_aliases key '" + key +
                                  "' resolves to nothing registered (typo, or the providing plugin is disabled)"));
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_naming_model() {
    std::vector<CheckResult> results;
    const auto& resolution = inputs_.name_resolution;

    if (!inputs_.skills.scan_error().empty()) {
        results.push_back(Warning("naming_model", "skills directory could not be scanned: " +
                                 inputs_.skills.scan_error()));
    }

    // The valid permission keys, and which bare names are ambiguous (two
    // plugins provide the original name -- the same count NameRegistry uses to
    // decide when a key must be qualified).
    std::unordered_set<std::string> valid_keys;
    for (const auto& entry : resolution.result.entries) valid_keys.insert(entry.permission_key);
    std::unordered_map<std::string, int> original_counts;
    for (const auto& attr : inputs_.tool_registry.attributions()) ++original_counts[attr.tool_name];
    const auto ambiguous = [&original_counts](const std::string& name) { return original_counts[name] > 1; };

    // per_tool_overrides keys (global and per-preset) must reference a permission key.
    const auto check_override_key = [&results, &valid_keys, &ambiguous](const std::string& where,
                                                                         const std::string& key) {
        if (ambiguous(key)) {
            results.push_back(Warning("naming_model", where + " per_tool_overrides key '" + key +
                                  "' is ambiguous -- two plugins provide that name; write it qualified as "
                                  "'<plugin>." + key + "'"));
        } else if (!valid_keys.count(key)) {
            results.push_back(Warning("naming_model", where + " per_tool_overrides key '" + key +
                                  "' resolves to no registered tool (typo, or the providing plugin is disabled)"));
        }
    };
    for (const auto& [key, mode] : permissions_.global_layer.per_tool_overrides) {
        check_override_key("permissions.json", key);
    }
    for (const auto& [preset_name, raw] : presets_) {
        for (const auto& [key, mode] : raw.permission_mode.per_tool_overrides) {
            check_override_key("agent preset '" + preset_name + "'", key);
        }
    }

    // allowed_tools entries reference permission keys too -- never aliases.
    for (const auto& [preset_name, raw] : presets_) {
        for (const auto& tool : raw.allowed_tools) {
            if (tool == "*" || valid_keys.count(tool)) continue;
            results.push_back(Warning("naming_model", "agent preset '" + preset_name +
                                  "': allowed_tools entry '" + tool + "' matches no registered tool"));
        }
    }

    // Unknown allowed_skills entries: the known gap this check closes -- they
    // are silently dropped by resolution, which is correct for lookup but must
    // be loud at startup.
    for (const auto& [preset_name, raw] : presets_) {
        for (const auto& skill : raw.allowed_skills) {
            if (skill == "*") continue;
            if (inputs_.skills.Find(skill) == nullptr) {
                results.push_back(Warning("naming_model", "agent preset '" + preset_name +
                                      "': allowed_skills entry '" + skill + "' is unknown"));
            }
            // A known-but-not-loaded candidate is reported once at skill level
            // below, with its load_error, rather than once per referencing preset.
        }
    }

    // Every not-loaded skill entry is itself a warning, referenced or not.
    for (const auto& meta : inputs_.skills.All()) {
        if (!meta.loaded) {
            results.push_back(Warning("naming_model", "skill '" + meta.name + "' not loaded: " + meta.load_error));
        }
    }

    // allowed_mcp_servers entries must name a configured server.
    for (const auto& [preset_name, raw] : presets_) {
        for (const auto& server : raw.allowed_mcp_servers) {
            if (mcps_.Find(server) == nullptr) {
                results.push_back(Warning("naming_model", "agent preset '" + preset_name +
                                      "': allowed_mcp_servers entry '" + server + "' matches no mcps.json server"));
            }
        }
    }

    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_advisor() {
    std::vector<CheckResult> results;
    if (permissions_.advisor) {
        const bool resolved = inputs_.permission_advisors != nullptr &&
                              inputs_.permission_advisors->Has(*permissions_.advisor);
        if (!resolved) {
            results.push_back(Warning(
                "advisor", "permissions.json names advisor '" + *permissions_.advisor +
                               "' but no loaded plugin registered one (typo, or the providing "
                               "plugin is disabled); prompts fall through to the human"));
        }
    } else if (permissions_.advisor_inherit) {
        results.push_back(Warning(
            "advisor", "permissions.json sets advisor_inherit but names no advisor; "
                       "the flag is inert"));
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_subagent_limit() {
    std::vector<CheckResult> results;
    const auto limit = settings_.max_concurrent_subagents;
    if (limit <= 0) {
        results.push_back(Warning("subagent_limit", "settings.json max_concurrent_subagents is " +
                                 std::to_string(limit) + "; spawn_subagent will always fail"));
    } else if (limit > 8) {
        // Warns, does not block: past 8 the task has outgrown the harness, but
        // that is the operator's call (ARCHITECTURE.md, "Subagents").
        results.push_back(Warning("subagent_limit", "settings.json max_concurrent_subagents is " +
                                 std::to_string(limit) + "; past 8 the task has outgrown the harness"));
    }
    return results;
}

}  // namespace minimal::config
