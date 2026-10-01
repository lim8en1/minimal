#include "minimal/client/tui_bootstrap.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

#include "minimal/client/config_file_io.hpp"
#include "minimal/client/key_event_parser.hpp"
#include "minimal/client/plugins/client_plugin_loader.hpp"
#include "minimal/client/plugins/core_registrations.hpp"
#include "minimal/client/plugins/host_seams.hpp"
#include "minimal/client/plugins/plugin_discovery.hpp"
#include "minimal/client/status_format.hpp"
#include "minimal/client/view/status_bar.hpp"
#include "minimal/common/bundled_assets.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/plugins/name_registry.hpp"

namespace minimal::client {

namespace {

// Left: what is happening and what it costs. Right ({>}): what it is running
// on. {version} and {session} are out of the default -- the banner/help carry
// the one, the tab bar the other -- but remain available ids.
constexpr const char* kDefaultStatusFormat =
    "{activity} · {tokens} · {cost?} · {token_detail?}{>}{model} · {agent} · {permission_mode} · "
    "{paused}";

}  // namespace

bool BuildTuiEnvironment(SessionClient& client, TuiEnvironment& out) {
    namespace plugins = minimal::client::plugins;

    // --- config + client plugins --------------------------------------------
    //
    // ORDER MATTERS, and each step is load-bearing for a later one:
    //   1. ui.json (status format -- it selects plugin segments by id)
    //   2. vendored grammars registered as the additive registry's defaults
    //   3. PLUGIN LOAD (contexts carry the attention sink and the data path;
    //      registries record what each plugin claimed)
    //   4. NameRegistry resolve: collisions fatal, dangling aliases warned
    //   5. THEME (sections may reference loaded plugin names)
    //   6. keybindings (+ plugin view defaults, only where nothing binds)
    //   7. core defaults fill every unclaimed slot, then the frame is assembled
    out.config_dir = minimal::common::ConfigDir();
    out.depth = minimal::client::DetectColorDepth();
    out.ui = minimal::client::LoadUiConfig(out.config_dir);

    // The seam objects must OUTLIVE plugin registration: a plugin captures the
    // raw pointer from its context at registration and may call it for the
    // process lifetime (lazy Find, attention raises). `out.attention_state`
    // is a member of `out` itself -- see the address-stability contract in
    // the header -- so `&out.attention_state` stays valid as long as `out`
    // is never relocated, which is the caller's obligation.
    out.host_focus_provider = plugins::MakeHostFocusProvider();   // also the HostUi seam
    out.attention_sink =
        plugins::MakeHostAttentionSink(&out.attention_state, out.host_focus_provider.get());
    out.host_client_api = plugins::MakeHostClientApi(&client);

    plugins::RegisterCoreSyntaxGrammars(out.registries.grammars);

    // Discovery + load. The config file is the client's own (client-plugins.json);
    // the server owns plugins.json, so neither writer can stomp the other's
    // entries. Written back after discovery, like the server's.
    plugins::ClientPluginConfig plugin_config;
    // Read once, reused below for the write-back comparison instead of a
    // second disk read: stays empty (matching "nothing on disk yet") when the
    // file doesn't exist, and is never consulted when plugin_config.malformed
    // ends up true (the write block is skipped in that case either way).
    std::string contents;
    {
        const auto config_path = out.config_dir / "client-plugins.json";
        std::error_code ec;
        if (std::filesystem::exists(config_path, ec)) {
            if (!TryReadConfigFile(config_path, &contents)) {
                // Exists but could not be read (permission, a transient I/O
                // error, a race with another process's own write): treated
                // exactly like a malformed parse below -- "we couldn't read
                // it" must never be silently treated as "it's empty," which
                // is what let the rewrite below replace the user's real
                // (merely unreadable) file with a fresh default.
                plugin_config.malformed = true;
                std::fprintf(stderr,
                             "minimal: warning: could not read %s -- it was NOT rewritten; "
                             "fix its permissions or remove it to enable plugin config "
                             "management\n",
                             std::filesystem::path(config_path).c_str());
            } else {
                plugin_config = plugins::ParseClientPluginConfig(contents);
                // ParseClientPluginConfig degrades a malformed file to an empty
                // config; silently rewriting the user's bytes over that default
                // would destroy their enablement decisions and per-plugin config
                // (a trailing comma would disable every user plugin on next
                // boot). On a malformed parse: warn and DO NOT rewrite.
                if (plugin_config.malformed) {
                    plugin_config.preserved = contents;
                    std::fprintf(stderr,
                                 "minimal: warning: %s is malformed -- it was NOT rewritten; "
                                 "fix or remove it to enable plugin config management\n",
                                 std::filesystem::path(config_path).c_str());
                }
            }
        }
        const auto candidates = plugins::DiscoverClientPlugins(
            minimal::common::BundledClientPluginDir(), out.config_dir / "client-plugins",
            plugin_config);
        if (!plugin_config.malformed) {
            // Same discipline as the server's WriteConfigIfChanged: create the
            // parent dir, skip the write when nothing changed, report errors.
            std::filesystem::create_directories(out.config_dir, ec);
            const auto serialized = plugins::SerializeClientPluginConfig(plugin_config);
            if (contents != serialized) {
                if (!WriteConfigFileAtomic(config_path, serialized)) {
                    std::fprintf(stderr, "minimal: warning: could not write %s\n",
                                 std::filesystem::path(config_path).c_str());
                }
            }
        }

        plugins::ClientPluginLoader loader(
            out.registries, {.attention = out.attention_sink.get(),
                             .client_api = out.host_client_api.get(),
                             .focus = out.host_focus_provider.get(),
                             .ui = out.host_focus_provider.get()});
        const auto records = loader.LoadAll(candidates);
        for (const auto& record : records) {
            if (record.ok()) out.loaded_plugins.push_back(record.name);
        }

        // Provenance-keyed severity, mirrored from the server's sanity check:
        // a failed BUNDLED plugin is fatal (a silently missing renderer reads
        // as a bug), a failed user plugin is a warning.
        bool fatal = false;
        for (const auto& record : records) {
            if (record.ok() || record.status == plugins::LoadStatus::Disabled) {
                if (record.status == plugins::LoadStatus::Disabled) continue;
                std::fprintf(stderr, "minimal: client plugin '%s' loaded (%s)\n",
                             record.name.c_str(), record.version.c_str());
                continue;
            }
            if (record.provenance == plugins::Provenance::Bundled) {
                std::fprintf(stderr, "minimal: FATAL: bundled client plugin '%s': %s (%s)\n",
                             record.file.string().c_str(), plugins::ToString(record.status).data(),
                             record.error.c_str());
                fatal = true;
            } else {
                std::fprintf(stderr, "minimal: warning: client plugin '%s': %s (%s)\n",
                             record.file.string().c_str(), plugins::ToString(record.status).data(),
                             record.error.c_str());
            }
        }
        if (fatal) return false;

        // Name collisions: the SAME rule as the server, through the shared
        // NameRegistry (moved to common so both loaders run one mechanism).
        // Registration keys are original names; a duplicate surface key is
        // fatal and names both claimants.
        minimal::plugins::NameRegistry names;
        const auto hooks = plugins::RegistryHooks(out.registries);
        std::vector<AliasCandidate> alias_candidates;
        for (const auto& hook : hooks) {
            if (!hook.name_kind) continue;   // keys that never collide
            for (const auto& attribution : hook.registry->attributions()) {
                names.Register(*hook.name_kind, attribution.plugin_name,
                               attribution.original_name);
            }
            if (hook.aliasable) {
                alias_candidates.push_back({*hook.name_kind, &hook.registry->attributions()});
            }
        }
        // Client aliases resolve collisions (the ARCHITECTURE.md rule: a
        // duplicate view id / tool name is "fatal and alias-resolvable"). Like
        // the server's resolver: the alias lands on the ONE kind whose stable
        // identity it names (so a resolved alias renames that entry's surface
        // and dedupes the collision), and an identity no kind claims is
        // dangling on a single kind -- reported once, not four times.
        // Addressing stays CANONICAL (keybindings.json addresses ids, never
        // aliases).
        for (const auto& [alias_key, alias_target] : plugin_config.aliases) {
            names.SetAlias(MatchAliasKind(alias_candidates, alias_key), alias_key, alias_target);
        }
        const auto resolved = names.Resolve();
        for (const auto& collision : resolved.fatal_collisions) {
            std::fprintf(stderr, "minimal: FATAL: client plugin name collision: %s\n",
                         collision.c_str());
            fatal = true;
        }
        for (const auto& dangling : resolved.dangling_aliases) {
            std::fprintf(stderr, "minimal: warning: client plugin alias names nothing: %s\n",
                         dangling.c_str());
        }
        if (fatal) return false;
    }

    // ui.json's "theme" selects the file (themes/<name>.json); empty means
    // the built-in default. Previously hardcoded to "dark" regardless of
    // config, which made the bundled light.json -- and any user theme --
    // unreachable from the TUI.
    const std::string theme_name = !out.ui.theme.empty() ? out.ui.theme : std::string("dark");
    out.theme.emplace([&] {
        try {
            return Theme::Load(out.config_dir / "themes", theme_name, out.depth,
                               &out.loaded_plugins);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "minimal: %s; using terminal defaults\n", e.what());
            // An empty-but-valid document: "no colors set" is the fallback,
            // and ColorFor's Default fallback covers every role. ("{}" is NOT
            // valid here -- Parse requires a "colors" object, so a missing
            // themes dir used to throw out of the fallback itself.)
            return Theme::Parse(R"({"name":"default","colors":{}})", out.depth);
        }
    }());
    Keybindings keys = [&] {
        try {
            return Keybindings::Load(out.config_dir);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "minimal: %s; using default keybindings\n", e.what());
            // Parse("{}") is an EMPTY table: without the default fill, a
            // malformed file silently unbound every core action (quit
            // included) while the message above claimed the opposite.
            Keybindings fallback = Keybindings::Parse("{}");
            fallback.ApplyDefaults();
            return fallback;
        }
    }();

    // Plugin views: register each id's suggested default keybinding only where
    // nothing binds it yet (the user's file wins), and remember the runtime
    // dispatch entries.
    // Core actions dispatch BEFORE plugin views (keybinding_dispatch.cpp), so
    // a view bound to a key a core action also uses can never be reached:
    // warn, naming both, instead of leaving a silently dead key.
    std::map<std::string, std::string> core_keys;   // normalized key -> core action
    for (const auto& [action, key] : keys.all()) {
        // Plugin view ids the user bound in keybindings.json are not core.
        if (key.empty() || out.registries.views.Find(action) != nullptr) continue;
        core_keys.emplace(NormalizeKeySpec(key), action);
    }
    for (const auto& view : out.registries.views.All()) {
        // A command's view_hint resolves the core overlays first
        // (TabMaintenance::HandleNotice), so a plugin view with one of their
        // names can never be opened by a hint.
        for (const char* core_view : {"sessions", "agents", "models", "subagent-models",
                                      "subagents"}) {
            if (view->spec().id == core_view) {
                std::fprintf(stderr,
                             "minimal: warning: plugin view '%s' shares a core view name; "
                             "command view hints open the core view instead\n",
                             core_view);
            }
        }
        keys.EnsureDefault(view->spec().id, view->spec().default_keybinding);
        const std::string bound = keys.KeyFor(view->spec().id);
        if (const auto clash = core_keys.find(NormalizeKeySpec(bound));
            !bound.empty() && clash != core_keys.end()) {
            std::fprintf(stderr,
                         "minimal: warning: plugin view '%s' is bound to '%s', which '%s' "
                         "already uses -- the view's key does nothing; rebind one in "
                         "keybindings.json\n",
                         view->spec().id.c_str(), bound.c_str(), clash->second.c_str());
        }
        // Plugin component code: a throw out of component() would propagate
        // into the frame assembly (and, later, the ftxui loop, where it would
        // std::terminate). Contained like every other plugin call site:
        // logged, that view skipped, never fatal.
        try {
            auto component = view->component();
            // A null component is skipped like a throwing one: every event
            // path downstream (keybinding dispatch, modal routing) calls
            // into it unguarded.
            if (component == nullptr) {
                common::DefaultLogger().warn("plugin view '{}' returned no component; skipped",
                                             view->spec().id);
                continue;
            }
            out.plugin_views.push_back(PluginViewEntry{view.get(), std::move(component), false});
        } catch (const std::exception& error) {
            common::DefaultLogger().warn("plugin view '{}' threw on component(): {}",
                                         view->spec().id, error.what());
        } catch (...) {
            common::DefaultLogger().warn(
                "plugin view '{}' threw on component() (non-standard exception)",
                view->spec().id);
        }
    }
    // Parsed ONCE, after keys.all() has settled (core defaults + the user's
    // file + every plugin view's EnsureDefault default, just folded in
    // above): the caller's with_keys used to re-derive this per EVENT --
    // MatchesAny(event, KeyToEvents(keys.KeyFor(action))) allocates a string
    // in KeyFor and a string+vector+Events in ParseKeySpec, for a table that
    // never changes again after startup. keymap.Unmapped() subsumes the
    // per-action warning loop this replaces.
    // Kept alongside the resolved (parsed-Event) form: the keymap help
    // overlay (help.overlay) needs the raw action->key STRINGS to display,
    // which ResolvedKeymap does not retain (it stores parsed Event
    // candidates, not the original spec strings).
    out.keybindings = keys;
    out.keymap = ResolvedKeymap::Build(out.keybindings);
    for (const auto& [action, key] : out.keymap.Unmapped()) {
        std::fprintf(stderr, "minimal: no key mapping for '%s' -> '%s'\n", action.c_str(),
                     key.c_str());
    }

    // The status format: ui.json's value, or the built-in default. Unknown ids
    // warn UNLESS a registered segment answers to them -- the registry is part
    // of the known-id set now.
    const std::string status_format_string =
        !out.ui.status_format.empty() ? out.ui.status_format : std::string(kDefaultStatusFormat);
    out.status_format = minimal::client::ParseStatusFormat(status_format_string);
    for (const auto& id : minimal::client::UnknownStatusSegments(status_format_string)) {
        if (out.registries.status_segments.Find(id) == nullptr) {
            std::fprintf(stderr, "minimal: unknown status segment '{%s}'\n", id.c_str());
        }
    }
    // Core segment ids are reserved: a plugin segment reusing one is ignored
    // (the frame skips it) -- it used to overwrite the core value silently.
    for (const auto& segment : out.registries.status_segments.All()) {
        if (minimal::client::IsKnownStatusSegment(segment->id())) {
            std::fprintf(stderr,
                         "minimal: warning: a plugin status segment reuses the core id "
                         "'{%s}'; the core segment is kept\n",
                         std::string(segment->id()).c_str());
        }
    }

    // The highlighter consults the assembled grammar table (vendored defaults
    // plus plugin additions), so a plugin's language highlights like a vendored
    // one. The renderer owns it; the engine routes fenced code through it.
    // Built INTO out.languages, not a local: SyntaxHighlighter stores a raw
    // pointer to this vector (see the address-stability contract in the
    // header), so its storage must outlive the highlighter, not just this
    // function call.
    for (const auto& grammar : out.registries.grammars.All()) {
        out.languages.push_back(minimal::client::LanguageDefinition{
            grammar->Language(), grammar->LanguageFn(), grammar->Rules()});
    }
    // emplace, not assignment: SyntaxHighlighter stores `const Theme&`, so it
    // has no default constructor and no operator= -- it can only ever be
    // constructed in place, once, with its final arguments (see the
    // address-stability contract in the header). `*out.theme` is already at
    // ITS final address (out.theme.emplace() ran above, on `out` itself), and
    // `out.languages` is too (populated in place, just above).
    out.highlighter.emplace(*out.theme, out.languages);

    // Core defaults fill every slot no plugin claimed. The engine first (the
    // agent-markdown default renders through it), then the kind defaults.
    // ONE construction of the renderer (emplace, same reasoning as
    // highlighter above: MarkdownRenderer stores raw Theme*/SyntaxHighlighter*
    // pointers, so it is built once with both dependencies already at their
    // final addresses inside `out`) -- the original inline code's own
    // comment recorded the exact bug a second construction would reintroduce:
    // "a refactor that introduced a second variable would dangle the engine
    // silently".
    out.renderer.emplace(&*out.theme, &*out.highlighter);
    plugins::RegisterDefaultMarkdownEngine(out.registries.markdown_engine, *out.renderer);
    // Non-null by construction: RegisterDefaultMarkdownEngine registers the
    // md4c adapter when no plugin claimed the slot.
    out.engine = out.registries.markdown_engine.get();
    plugins::RegisterCoreMessageRenderers(out.registries.message_renderers);
    plugins::RegisterCoreAgentMarkdownRenderer(out.registries.message_renderers, *out.engine);
    plugins::RegisterCoreDisplayRenderers(out.registries.display_renderers, *out.engine, *out.highlighter);

    // The exclusive chat surface, when a plugin claimed it.
    out.chat_surface = out.registries.chat_surface.get();

    // Both pushes below (chat surface + views) live in PushThemeToPlugins so
    // RunTui's /theme swap can re-run the identical sequence after replacing
    // out.theme's contents in place.
    PushThemeToPlugins(out);

    // Registration window closes here: every plugin's register_* hook (and
    // core's own registrations) has run. A stashed registry reference that
    // registers after this point warns and is ignored instead of silently
    // bypassing the collision and attribution rules.
    for (const auto& hook : plugins::RegistryHooks(out.registries)) hook.registry->Seal();

    return true;
}

void PushThemeToPlugins(TuiEnvironment& out) {
    // The exclusive chat surface, when a plugin claimed it. A throwing
    // surface is disabled, not fatal (catch + disable, track R2): OnTheme
    // runs before any event flows, so a throw here is the plugin's first
    // chance to kill the process unguarded.
    //
    // Logged, not fprintf'd: this function also runs from RunTui's /theme
    // SWAP path (app.cpp), while the alt screen is live -- a raw
    // fprintf(stderr) there garbles the running display. The logger is safe
    // here too, on the very first (pre-TUI) call: it's the same sink every
    // other plugin-facing guard in this bootstrap window already uses (e.g.
    // registries.cpp's GuardedRegistration warnings).
    if (out.chat_surface != nullptr) {
        try {
            out.chat_surface->OnTheme(*out.theme);
        } catch (const std::exception& error) {
            common::DefaultLogger().error(
                "chat surface plugin threw on theme ({}) -- disabled; the core chat view "
                "takes over",
                error.what());
            out.chat_surface = nullptr;
        } catch (...) {
            common::DefaultLogger().error(
                "chat surface plugin threw on theme (non-standard exception) -- disabled; "
                "the core chat view takes over");
            out.chat_surface = nullptr;
        }
    }

    // Views get the same one-shot theme push ChatSurface does (default
    // no-op; a view that threw is logged, not disabled -- its component may
    // still be harmless, and panels are contained at render time anyway).
    // StartView needs no push: its Render() takes the theme as a parameter.
    for (const auto& view : out.registries.views.All()) {
        try {
            view->OnTheme(*out.theme);
        } catch (const std::exception& error) {
            common::DefaultLogger().warn("view plugin '{}' threw on theme: {}",
                                         view->spec().id, error.what());
        } catch (...) {
            common::DefaultLogger().warn(
                "view plugin '{}' threw on theme (non-standard exception)", view->spec().id);
        }
    }
}

minimal::plugins::NameKind MatchAliasKind(const std::vector<AliasCandidate>& candidates,
                                          const std::string& key) {
    for (const auto& candidate : candidates) {
        for (const auto& a : *candidate.attributions) {
            if (a.plugin_name + "." + a.original_name == key) return candidate.kind;
        }
    }
    return minimal::plugins::NameKind::ViewId;   // dangling
}

}  // namespace minimal::client
