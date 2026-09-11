#include "minimal/client/tui_bootstrap.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#include "minimal/client/config_file_io.hpp"
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
    "{activity} · {tokens} · {cost} · {token_detail}{>}{model} · {agent} · {permission_mode}";

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
    out.attention_sink = plugins::MakeHostAttentionSink(&out.attention_state);
    out.host_client_api = plugins::MakeHostClientApi(&client);
    out.host_focus_provider = plugins::MakeHostFocusProvider();

    plugins::RegisterCoreSyntaxGrammars(out.grammar_registry);

    // Discovery + load. The config file is the client's own (client-plugins.json);
    // the server owns plugins.json, so neither writer can stomp the other's
    // entries. Written back after discovery, like the server's.
    plugins::ClientPluginConfig plugin_config;
    {
        const auto config_path = out.config_dir / "client-plugins.json";
        std::error_code ec;
        if (std::filesystem::exists(config_path, ec)) {
            std::string contents;
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
            std::string existing;
            bool existing_known = true;
            if (std::filesystem::exists(config_path, ec)) {
                existing_known = TryReadConfigFile(config_path, &existing);
            }
            // existing_known false means the file exists but could not be
            // read just now (a race since the read above, or a permission
            // change) -- skip the write rather than risk clobbering real
            // bytes we never actually saw.
            if (existing_known && existing != serialized) {
                if (!WriteConfigFileAtomic(config_path, serialized)) {
                    std::fprintf(stderr, "minimal: warning: could not write %s\n",
                                 std::filesystem::path(config_path).c_str());
                }
            }
        }

        plugins::ClientPluginLoader loader(out.view_registry, out.renderer_registry,
                                           out.display_registry, out.message_registry,
                                           out.status_registry,
                                           out.grammar_registry, out.surface_registry,
                                           out.engine_registry, out.start_view_registry,
                                           out.attention_sink.get(), out.host_client_api.get(),
                                           out.host_focus_provider.get());
        const auto records = loader.LoadAll(candidates);

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
        const auto feed = [&names](minimal::plugins::NameKind kind,
                                   const std::vector<plugins::Attribution>& attributions) {
            for (const auto& attribution : attributions) {
                names.Register(kind, attribution.plugin_name, attribution.original_name);
            }
        };
        feed(minimal::plugins::NameKind::ViewId, out.view_registry.attributions());
        feed(minimal::plugins::NameKind::ViewId, out.start_view_registry.attributions());
        feed(minimal::plugins::NameKind::ToolResultRenderer, out.renderer_registry.attributions());
        feed(minimal::plugins::NameKind::DisplayRenderer, out.display_registry.attributions());
        for (const auto& [kind, attribution] : out.message_registry.claims()) {
            names.Register(minimal::plugins::NameKind::MessageRenderer,
                           attribution.plugin_name, attribution.original_name);
        }
        feed(minimal::plugins::NameKind::StatusSegment, out.status_registry.attributions());
        feed(minimal::plugins::NameKind::SyntaxGrammar, out.grammar_registry.attributions());
        // Client aliases resolve collisions (the ARCHITECTURE.md rule: a
        // duplicate view id / tool name is "fatal and alias-resolvable"). They
        // were parsed, serialized and documented while never reaching the
        // NameRegistry -- so the alias key did nothing and the dangling-alias
        // diagnostic below was unreachable. Like the server's resolver: the
        // alias lands on the ONE kind whose stable identity it names (so a
        // resolved alias renames that entry's surface and dedupes the
        // collision), and an identity no kind claims is dangling on a single
        // kind -- reported once, not four times. Addressing stays CANONICAL
        // (keybindings.json addresses ids, never aliases).
        const std::vector<AliasCandidate> alias_candidates = {
            {minimal::plugins::NameKind::ViewId, &out.view_registry.attributions()},
            {minimal::plugins::NameKind::ViewId, &out.start_view_registry.attributions()},
            {minimal::plugins::NameKind::ToolResultRenderer, &out.renderer_registry.attributions()},
            {minimal::plugins::NameKind::DisplayRenderer, &out.display_registry.attributions()},
            {minimal::plugins::NameKind::StatusSegment, &out.status_registry.attributions()},
            {minimal::plugins::NameKind::SyntaxGrammar, &out.grammar_registry.attributions()},
        };
        for (const auto& [alias_key, alias_target] : plugin_config.aliases) {
            const auto kind = MatchAliasKind(alias_candidates, out.message_registry.claims(), alias_key);
            names.SetAlias(kind, alias_key, alias_target);
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

    // Loaded plugin names feed the theme's section check.
    std::vector<std::string> loaded_plugins;
    const auto collect_loaded = [&loaded_plugins](const std::vector<plugins::Attribution>& attributions) {
        for (const auto& a : attributions) {
            if (!a.plugin_name.empty()) loaded_plugins.push_back(a.plugin_name);
        }
    };
    collect_loaded(out.view_registry.attributions());
    collect_loaded(out.start_view_registry.attributions());
    collect_loaded(out.renderer_registry.attributions());
    collect_loaded(out.display_registry.attributions());
    collect_loaded(out.status_registry.attributions());
    collect_loaded(out.grammar_registry.attributions());

    // ui.json's "theme" selects the file (themes/<name>.json); empty means
    // the built-in default. Previously hardcoded to "dark" regardless of
    // config, which made the bundled light.json -- and any user theme --
    // unreachable from the TUI.
    const std::string theme_name = !out.ui.theme.empty() ? out.ui.theme : std::string("dark");
    out.theme.emplace([&] {
        try {
            return Theme::Load(out.config_dir / "themes", theme_name, out.depth, &loaded_plugins);
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
    for (const auto& view : out.view_registry.views()) {
        keys.EnsureDefault(view->spec().id, view->spec().default_keybinding);
        // Plugin component code: a throw out of component() would propagate
        // into the frame assembly (and, later, the ftxui loop, where it would
        // std::terminate). Contained like every other plugin call site:
        // logged, that view skipped, never fatal.
        try {
            out.plugin_views.push_back(PluginViewEntry{view.get(), view->component(), false});
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
        if (out.status_registry.Find(id) == nullptr) {
            std::fprintf(stderr, "minimal: unknown status segment '{%s}'\n", id.c_str());
        }
    }

    // The highlighter consults the assembled grammar table (vendored defaults
    // plus plugin additions), so a plugin's language highlights like a vendored
    // one. The renderer owns it; the engine routes fenced code through it.
    // Built INTO out.languages, not a local: SyntaxHighlighter stores a raw
    // pointer to this vector (see the address-stability contract in the
    // header), so its storage must outlive the highlighter, not just this
    // function call.
    for (const auto& grammar : out.grammar_registry.grammars()) {
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
    plugins::RegisterDefaultMarkdownEngine(out.engine_registry, *out.renderer);
    // Non-null by construction: RegisterDefaultMarkdownEngine registers the
    // md4c adapter when no plugin claimed the slot.
    out.engine = out.engine_registry.engine.get();
    plugins::RegisterCoreMessageRenderers(out.message_registry);
    plugins::RegisterCoreAgentMarkdownRenderer(out.message_registry, *out.engine);
    plugins::RegisterCoreDisplayRenderers(out.display_registry, *out.engine, *out.highlighter);

    // The exclusive chat surface, when a plugin claimed it.
    out.chat_surface = out.surface_registry.surface.get();

    // Both pushes below (chat surface + views) live in PushThemeToPlugins so
    // RunTui's /theme swap can re-run the identical sequence after replacing
    // out.theme's contents in place.
    PushThemeToPlugins(out);

    // Registration window closes here: every plugin's register_* hook (and
    // core's own registrations) has run. A stashed registry reference that
    // registers after this point warns and is ignored instead of silently
    // bypassing the collision and attribution rules.
    out.view_registry.Seal();
    out.start_view_registry.Seal();
    out.renderer_registry.Seal();
    out.display_registry.Seal();
    out.message_registry.Seal();
    out.status_registry.Seal();
    out.grammar_registry.Seal();
    out.surface_registry.Seal();
    out.engine_registry.Seal();

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
    for (const auto& view : out.view_registry.views()) {
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

minimal::plugins::NameKind MatchAliasKind(
    const std::vector<AliasCandidate>& candidates,
    const std::vector<std::pair<ChatRowKind, plugins::Attribution>>& message_claims,
    const std::string& key) {
    const auto matches = [](const std::vector<plugins::Attribution>& attributions,
                            const std::string& qualified) {
        for (const auto& a : attributions) {
            if (a.plugin_name + "." + a.original_name == qualified) return true;
        }
        return false;
    };
    for (const auto& candidate : candidates) {
        if (matches(*candidate.attributions, key)) return candidate.kind;
    }
    // message_claims is (kind, attribution) pairs, a different shape from
    // the others' plain attributions() -- stays its own check.
    for (const auto& [kind, attribution] : message_claims) {
        (void)kind;
        if (attribution.plugin_name + "." + attribution.original_name == key) {
            return minimal::plugins::NameKind::MessageRenderer;
        }
    }
    return minimal::plugins::NameKind::ViewId;   // dangling
}

}  // namespace minimal::client
