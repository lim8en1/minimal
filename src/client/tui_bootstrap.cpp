#include "minimal/client/tui_bootstrap.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <system_error>
#include <vector>

#include "minimal/client/config_file_io.hpp"
#include "minimal/client/sdk/key_event_parser.hpp"
#include "minimal/client/plugins/client_plugin_loader.hpp"
#include "minimal/client/plugins/core_registrations.hpp"
#include "minimal/client/plugins/host_seams.hpp"
#include "minimal/client/plugins/plugin_discovery.hpp"
#include "minimal/client/view/status_bar.hpp"
#include "minimal/common/bundled_assets.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/plugins/name_table.hpp"

namespace minimal::client {

bool LoadClientPluginEnvironment(SessionClient* client, TuiEnvironment& out,
                                 bool announce_loads, bool write_back) {
    namespace plugins = minimal::client::plugins;

    // --- config + client plugins --------------------------------------------
    //
    // ORDER MATTERS, and each step is load-bearing for a later one:
    //   1. ui.json (status_segments -- it moves plugin segments by id)
    //   2. (the grammars are plugins' too: the bundled syntax-grammars)
    //   3. PLUGIN LOAD (contexts carry the attention sink and the data path;
    //      registries record what each plugin claimed)
    //   4. Name resolution: collisions fatal, dangling/slot aliases warned
    //      -- then required slots must be filled (step 6 builds plugin view
    //      components, which may use the widget kit)
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
    out.host_client_api = plugins::MakeHostClientApi(client);   // null: answers nothing
    out.host_widget_kit = plugins::MakeHostWidgetKit(out.registries.widget_kit);
    out.host_highlighter = std::make_unique<plugins::HostCodeHighlighter>();
    out.host_markdown = plugins::MakeHostMarkdownEngine(out.registries.markdown_engine);
    // Plugins' kept state (HostUi::SaveState), beside the client's own data.
    try {
        out.host_focus_provider->SetStateDir(common::DataDir() / "client-plugin-state");
    } catch (const std::exception&) {   // no $HOME: nothing kept across runs
    }
    out.host_actions = std::make_unique<plugins::HostActionQueue>(out.host_focus_provider.get());


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
            plugin_config, out.config_dir / minimal::common::kClientPluginConfigDirName);
        // An entry with a problem was dropped from the parsed config: warn,
        // and never rewrite (the rewrite would delete the user's entry).
        for (const auto& problem : plugin_config.problems) {
            std::fprintf(stderr, "minimal: warning: %s (the file was not rewritten)\n",
                         problem.c_str());
        }
        if (write_back && !plugin_config.malformed && plugin_config.problems.empty()) {
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
                             .ui = out.host_focus_provider.get(),
                             .widgets = out.host_widget_kit.get(),
                             .actions = out.host_actions.get(),
                             .highlighter = out.host_highlighter.get(),
                             .markdown = out.host_markdown.get(),
                             .ui_for_plugin = [provider = out.host_focus_provider.get()](
                                                  const std::string& plugin_name) {
                                 return provider->UiFor(plugin_name);
                             }});
        const auto records = loader.LoadAll(candidates);
        for (const auto& record : records) {
            if (record.ok()) out.loaded_plugins.push_back(record.name);
            plugins::ClientPluginInfo info;
            info.id = record.file.filename().string();
            info.name = record.name.empty() ? record.file.stem().string() : record.name;
            info.enabled = record.status != plugins::LoadStatus::Disabled;
            info.enabled_in_file = info.enabled;
            info.loaded = record.ok();
            if (!record.ok() && info.enabled) info.error = record.error;
            for (const auto& [label, names] : record.registered) {
                auto& distinct = info.provides[label];
                for (const auto& name : names) {
                    if (std::find(distinct.begin(), distinct.end(), name) == distinct.end()) {
                        distinct.push_back(name);
                    }
                }
                if (distinct.empty()) info.provides.erase(label);
            }
            out.client_plugin_provenance[info.id] = record.provenance;
            out.client_plugins.push_back(std::move(info));
        }

        // Provenance-keyed severity, mirrored from the server's sanity check:
        // a failed BUNDLED plugin is fatal (a silently missing renderer reads
        // as a bug), a failed user plugin is a warning.
        bool fatal = false;
        for (const auto& record : records) {
            if (record.ok() || record.status == plugins::LoadStatus::Disabled) {
                if (record.status == plugins::LoadStatus::Disabled || !announce_loads) continue;
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

        // Name collisions: the SAME mechanism as the server (common's NameTable),
        // with the client's namespaces and policy from the hook table.
        std::vector<NameFeed> feeds;
        for (const auto& hook : plugins::RegistryHooks(out.registries)) {
            if (hook.name_space.empty()) continue;  // keys that never collide
            feeds.push_back({hook.name_space, hook.policy, &hook.registry->attributions()});
        }
        const auto names = ResolveClientNames(feeds, plugin_config.aliases);
        for (const auto& message : names.fatal) {
            std::fprintf(stderr, "minimal: FATAL: %s\n", message.c_str());
            fatal = true;
        }
        for (const auto& message : names.warnings) {
            std::fprintf(stderr, "minimal: warning: %s\n", message.c_str());
        }
        if (fatal) return false;

        // Collisions resolved: rename what the aliases rename. Names only --
        // a slot registry is never renamed (ResolveClientNames warned).
        for (const auto& hook : plugins::RegistryHooks(out.registries)) {
            if (hook.name_space.empty() ||
                hook.policy != minimal::plugins::NameTable::Policy::kName) {
                continue;
            }
            hook.registry->ApplyAliases(plugin_config.aliases);
        }
    }

    // A required slot no plugin filled: the TUI cannot run without it (the
    // server's "no provider" rule). Checked here, before anything -- the
    // plugin views' component()s, built with the keybindings below -- can
    // ask for what it would have provided.
    {
        bool missing = false;
        for (const auto& hook : plugins::RegistryHooks(out.registries)) {
            if (!hook.required || !hook.registry->empty()) continue;
            std::fprintf(stderr,
                         "minimal: FATAL: no client plugin fills the required '%s' slot -- is "
                         "the bundled plugin that provides it disabled in client-plugins.json?\n",
                         std::string(hook.label).c_str());
            missing = true;
        }
        // Core's own questions are prompt KINDS, not a slot: a permission
        // request or a quit confirmation with no renderer could only be
        // answered "no" -- refuse instead of running a TUI that cannot ask.
        for (const char* kind : {"permission", "confirmation"}) {
            if (out.registries.prompt_renderers.Find(kind) != nullptr) continue;
            std::fprintf(stderr,
                         "minimal: FATAL: no client plugin renders the required '%s' prompt kind "
                         "-- is the bundled consent-prompts plugin disabled in "
                         "client-plugins.json?\n",
                         kind);
            missing = true;
        }
        if (missing) return false;

        // What the TUI cannot start without, by holder: such a plugin is
        // never toggled off from /config.
        const auto require = [&out](const std::string& holder, const std::string& why) {
            for (auto& info : out.client_plugins) {
                if (info.name == holder && info.required.empty()) info.required = why;
            }
        };
        require(out.registries.chat_surface.holder(), "it draws the chat");
        require(out.registries.prompt_editor.holder(), "it is the prompt editor");
        require(out.registries.widget_kit.holder(), "every view's widgets come from it");
        const auto& kinds = out.registries.prompt_renderers;
        for (std::size_t i = 0; i < kinds.All().size(); ++i) {
            const std::string& kind = kinds.surfaces()[i];
            if (kind == "permission" || kind == "confirmation") {
                require(kinds.owners()[i], "it asks for permissions and confirmations");
            }
        }
    }

    // The status bar: every segment where its plugin placed it, ui.json's
    // exceptions applied. An override naming no segment is the user's typo
    // or a disabled plugin -- said once, then ignored.
    out.status_bar = ComposeStatusBar(out.registries.status_segments, out.ui.status_segments);
    for (const auto& id : out.status_bar.unknown) {
        std::fprintf(stderr,
                     "minimal: warning: ui.json: status_segments.%s: no status segment by that "
                     "name -- ignored\n",
                     id.c_str());
    }
    out.status_segment_ids.clear();
    for (const auto& piece : out.status_bar.format) {
        if (piece.is_segment && piece.text != kStatusSplitId) out.status_segment_ids.insert(piece.text);
    }

    // The markdown engine is a plugin's (the bundled markdown-md4c); with
    // none, markdown reads as plain text -- core's own registration, the
    // last one.
    plugins::RegisterPlainMarkdownEngine(out.registries.markdown_engine);

    // Registration window closes here, before any plugin code beyond the
    // register_* hooks runs (a slot's Actions(), a view's component(),
    // OnTheme): a stashed registry reference that registers from there warns
    // and is ignored, instead of being recorded as core's and bypassing the
    // collision and attribution rules.
    for (const auto& hook : plugins::RegistryHooks(out.registries)) hook.registry->Seal();
    return true;
}

bool BuildTuiEnvironment(SessionClient& client, TuiEnvironment& out) {
    namespace plugins = minimal::client::plugins;
    if (!LoadClientPluginEnvironment(&client, out, /*announce_loads=*/true)) return false;

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

    // The prompt editor's own actions (the required slot is filled by now):
    // their defaults, where the user's file binds nothing. Core actions never
    // share their ids (by convention "prompt.<name>").
    // Plugin code, and both slots are required: a throw is fatal, but named
    // -- never an escape that ends the process with no word of which plugin.
    std::set<std::string> slot_actions;   // for the unknown-id check below
    const auto slot_actions_of = [&](const char* slot, const std::string& holder,
                                     const auto& actions_fn) {
        try {
            for (const auto& action : actions_fn()) {
                // A slot's default on a key something else already holds:
                // only one fires (ApplyDefaults warns the same for core's).
                std::string clash;
                if (keys.EnsureDefault(action.id, action.default_key, &clash) && !clash.empty()) {
                    std::fprintf(stderr,
                                 "minimal: warning: the %s's default '%s' for '%s' is also bound "
                                 "to '%s' -- only one can fire; rebind one in keybindings.json\n",
                                 slot, action.default_key.c_str(), action.id.c_str(), clash.c_str());
                }
                slot_actions.insert(action.id);
            }
            return true;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "minimal: FATAL: client plugin '%s' (the %s) threw on Actions(): %s\n",
                         holder.c_str(), slot, error.what());
        } catch (...) {
            std::fprintf(stderr, "minimal: FATAL: client plugin '%s' (the %s) threw on Actions()\n",
                         holder.c_str(), slot);
        }
        return false;
    };
    if (!slot_actions_of("prompt editor", out.registries.prompt_editor.holder(),
                         [&] { return out.registries.prompt_editor.get()->Actions(); }) ||
        // The chat surface's too ("chat.<name>").
        !slot_actions_of("chat surface", out.registries.chat_surface.holder(),
                         [&] { return out.registries.chat_surface.get()->Actions(); })) {
        return false;
    }

    // Plugin views: register each id's suggested default keybinding only where
    // nothing binds it yet (the user's file wins), and remember the runtime
    // dispatch entries.
    // Core actions dispatch BEFORE plugin views (keybinding_dispatch.cpp), so
    // a view bound to a key a core action also uses can never be reached:
    // warn, naming both, instead of leaving a silently dead key.
    std::map<std::string, std::string> core_keys;   // normalized key -> core action
    for (const auto& [action, key] : keys.all()) {
        // Only what dispatches before a view: core's actions and the slots'
        // declared ones. A view id is not core, and an id nothing answers to
        // (a renamed action) holds no key -- it gets its own warning below.
        if (key.empty() || (!Keybindings::IsCoreAction(action) && slot_actions.count(action) == 0)) {
            continue;
        }
        core_keys.emplace(NormalizeKeySpec(key), action);
    }
    const auto& views = out.registries.views.All();
    const auto& view_surfaces = out.registries.views.surfaces();
    for (std::size_t i = 0; i < views.size(); ++i) {
        const auto& view = views[i];
        // Users address the SURFACE id (the alias, if one renamed the view).
        const std::string& surface = view_surfaces[i];
        // Plugin code (spec(), component()): a throw would propagate into the
        // frame assembly (and, later, the ftxui loop, where it would
        // std::terminate). Contained like every other plugin call site:
        // logged, that view skipped, never fatal.
        try {
            keys.EnsureDefault(surface, view->spec().default_keybinding);
            const std::string bound = keys.KeyFor(surface);
            if (const auto clash = core_keys.find(NormalizeKeySpec(bound));
                !bound.empty() && clash != core_keys.end()) {
                std::fprintf(stderr,
                             "minimal: warning: plugin view '%s' is bound to '%s', which '%s' "
                             "already uses -- the view's key does nothing; rebind one in "
                             "keybindings.json\n",
                             surface.c_str(), bound.c_str(), clash->second.c_str());
            }
            auto component = view->component();
            // A null component is skipped like a throwing one: every event
            // path downstream (keybinding dispatch, modal routing) calls
            // into it unguarded.
            if (component == nullptr) {
                common::DefaultLogger().warn("plugin view '{}' returned no component; skipped",
                                             surface);
                continue;
            }
            PluginViewEntry entry{view.get(), std::move(component), false, false, surface, {}};
            entry.plugin = out.registries.views.owners()[i];
            out.plugin_views.push_back(std::move(entry));
        } catch (const std::exception& error) {
            common::DefaultLogger().warn("plugin view '{}' threw on component(): {}", surface,
                                         error.what());
        } catch (...) {
            common::DefaultLogger().warn(
                "plugin view '{}' threw on component() (non-standard exception)", surface);
        }
    }
    // A role core opens is filled by one view: a second claimant is never
    // reached, so say which one stands.
    {
        std::map<std::string, std::string> roles;   // role -> the surface filling it
        for (const auto& entry : out.plugin_views) {
            const std::string& role = entry.view->spec().role;
            if (role.empty()) continue;
            const auto [it, fresh] = roles.emplace(role, entry.surface_id);
            if (!fresh) {
                std::fprintf(stderr,
                             "minimal: warning: views '%s' and '%s' both fill the '%s' role -- "
                             "'%s' is the one core opens\n",
                             it->second.c_str(), entry.surface_id.c_str(), role.c_str(),
                             it->second.c_str());
            }
        }
    }

    // A binding nothing in this build answers to -- a renamed action, a view
    // whose plugin is disabled, a typo -- is kept (the file may predate or
    // postdate the build) but said out loud: it used to sit dead silently.
    {
        std::vector<std::string> unknown;
        for (const auto& [action, key] : keys.all()) {
            if (Keybindings::IsCoreAction(action) || slot_actions.count(action) != 0 ||
                out.registries.views.Find(action) != nullptr) {
                continue;
            }
            unknown.push_back(action);
        }
        std::sort(unknown.begin(), unknown.end());
        for (const auto& action : unknown) {
            std::fprintf(stderr,
                         "minimal: warning: keybindings.json binds '%s', which nothing in this "
                         "build answers to -- renamed, a disabled plugin's view, or a typo?\n",
                         action.c_str());
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

    // The highlighter consults the assembled grammar table (every
    // registered grammar -- the bundled syntax-grammars plugin's and
    // any other plugin's alike). The renderer owns it; the engine routes fenced code through it.
    // Built INTO out.languages, not a local: SyntaxHighlighter stores a raw
    // pointer to this vector (see the address-stability contract in the
    // header), so its storage must outlive the highlighter, not just this
    // function call.
    // Plugin code, so contained like every other additive entry: a grammar
    // whose accessors throw is skipped (its fences render plain), never the
    // end of startup.
    const auto& grammar_owners = out.registries.grammars.owners();
    for (std::size_t i = 0; i < out.registries.grammars.All().size(); ++i) {
        const auto& grammar = out.registries.grammars.All()[i];
        try {
            out.languages.push_back(minimal::client::LanguageDefinition{
                grammar->Language(), grammar->LanguageFn(), grammar->Rules(),
                grammar->FunctionFields(), grammar->Aliases()});
        } catch (...) {
            std::fprintf(stderr,
                         "minimal: warning: a syntax grammar of plugin '%s' threw; skipped\n",
                         grammar_owners[i].c_str());
        }
    }
    // emplace, not assignment: SyntaxHighlighter stores `const Theme&`, so it
    // has no default constructor and no operator= -- it can only ever be
    // constructed in place, once, with its final arguments (see the
    // address-stability contract in the header). `*out.theme` is already at
    // ITS final address (out.theme.emplace() ran above, on `out` itself), and
    // `out.languages` is too (populated in place, just above).
    out.highlighter.emplace(*out.theme, out.languages);
    // Plugins hold the proxy from registration; it reaches the real one now.
    out.host_highlighter->Attach(&*out.highlighter);

    // Non-null by construction: a plugin's engine, or the plain-text one
    // LoadClientPluginEnvironment registered.
    out.engine = out.registries.markdown_engine.get();

    // The chat surface (required: present by now).
    out.chat_surface = out.registries.chat_surface.get();

    // The views' theme push lives in PushThemeToPlugins so
    // RunTui's /theme swap can re-run the identical sequence after replacing
    // out.theme's contents in place.
    PushThemeToPlugins(out);

    return true;
}

void PushThemeToPlugins(TuiEnvironment& out) {
    // Views get a one-shot theme push (default no-op; a view that threw is logged, not disabled -- its component may
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

ClientNameResolution ResolveClientNames(const std::vector<NameFeed>& feeds,
                                        const std::map<std::string, std::string>& aliases) {
    using minimal::plugins::NameTable;
    // One table per namespace, in first-appearance order (std::map would
    // reorder the diagnostics alphabetically by namespace label; either is
    // stable, this one follows the hook table).
    std::vector<std::pair<std::string_view, NameTable>> tables;
    const auto table_for = [&tables](const NameFeed& feed) -> NameTable& {
        for (auto& [name_space, table] : tables) {
            if (name_space == feed.name_space) return table;
        }
        return tables.emplace_back(feed.name_space, NameTable(feed.policy)).second;
    };
    for (const auto& feed : feeds) {
        NameTable& table = table_for(feed);
        for (const auto& attribution : *feed.attributions) {
            table.Register(attribution.plugin_name, attribution.original_name);
        }
    }
    std::vector<NameTable*> table_ptrs;
    for (auto& [name_space, table] : tables) table_ptrs.push_back(&table);

    ClientNameResolution resolution;
    const auto routing = minimal::plugins::RouteAliases(aliases, table_ptrs);
    for (const auto& key : routing.dangling) {
        resolution.warnings.push_back("client plugin alias '" + key + "' names nothing registered");
    }
    for (const auto& key : routing.targets_slot) {
        resolution.warnings.push_back(
            "client plugin alias '" + key +
            "' names a renderer, grammar or command, which cannot be aliased (aliases rename "
            "view ids and status segments) -- ignored");
    }
    for (const auto& [name_space, table] : tables) {
        const auto result = table.Resolve();
        for (const auto& collision : result.collisions) {
            std::string owners;
            for (const auto& plugin_name : collision.claimants) {
                if (!owners.empty()) owners += ", ";
                owners += plugin_name.empty() ? "core" : "'" + plugin_name + "'";
            }
            resolution.fatal.push_back(
                "client plugin " + std::string(name_space) + " '" + collision.surface +
                "' is registered by " + owners +
                (table.policy() == NameTable::Policy::kName
                     ? " -- disable one plugin, or alias one of them in client-plugins.json"
                     : " -- disable one of the plugins"));
        }
        for (const auto& identity : result.ambiguous_identities) {
            resolution.fatal.push_back("client plugin " + std::string(name_space) + " identity '" +
                                       identity + "' is composed ambiguously (a plugin name "
                                       "containing a dot) -- rename one of the plugins");
        }
    }
    return resolution;
}

}  // namespace minimal::client
