# Plugin reference

Nearly everything interesting in minimal ships as a plugin using the same shape
a third-party plugin would: read/write/edit/bash, the OpenAI and Anthropic
providers, and the built-in `/`-commands are all bundled plugins, not
privileged code paths. Core owns *mechanisms*; plugins own the rest.

Plugins are shared libraries (`.so` / `.dylib`) loaded at startup, guarded by
an **ABI tag** (`<compiler-id>-<compiler-version>-cxx20-<system>` baked into
both host and plugin — a mismatch refuses to load). They are source-compatible,
not binary-compatible: rebuild them with the harness.

## Bundled server plugins (`plugins/server/`)

Installed to `<libdir>/minimal/plugins/`. All enabled by default unless noted.

| Plugin | Provides |
|---|---|
| `tools-fs` | Tools `read`, `write`, `edit` |
| `tools-search` | Tools `grep`, `glob` (RE2-backed, linear-time regex) |
| `tools-bash` | Tool `bash`; commands `/restart-shell`, `/tasks`; hook point `before_command`; per-session cwd/env retention |
| `tools-web` | Tools `web_fetch`, `web_search` |
| `provider-openai` | Provider kind `openai` (chat-completions shape) + model catalog for it |
| `provider-anthropic` | Provider kind `anthropic` (Messages API, prompt caching) |
| `commands-core` | Commands `/help`, `/clear`, `/compact`, `/agent`, `/model`, `/permission-mode`, `/sessions` |
| `checklist` | Tool `checklist` + cross-plugin RPC endpoint `checklist.rpc` |
| `signals-message` | Tool `message_session` (cross-session messaging on the core `signals` service) |
| `cost-tracker` | Commands `/cost`, `/cost-update-rates`; per-session cost accumulation; RPC endpoint `cost-tracker.rpc` |
| `session-titler` | Session observer that auto-titles sessions (~100 tokens/session spend). **Ships disabled** |

## Bundled client plugins (`plugins/client/`)

Installed to `<libdir>/minimal/plugins/client/`.

| Plugin | Provides |
|---|---|
| `logo` | The ASCII-banner start view (random banner per session) |
| `cost-view` | The `{cost}` status-bar segment (polls `cost-tracker.rpc`, renders cumulative $ / tokens) |

## Discovery, enablement, replacement

Two directories are scanned at startup:

- **Bundled dir** — resolved relative to the running executable (the install
  layout above; a build-tree fallback also works). Scanned recursively.
- **User dir** — `<config>/plugins/` (server) and `<config>/client-plugins/`
  (client), scanned flat. Drop `.so`/`.dylib` files here to install.

Enablement is recorded in `<config>/plugins.json` (client:
`<config>/client-plugins.json`), written automatically at each startup:

```json
{
  "plugins": [
    { "file": "tools-fs.so", "provenance": "bundled", "enabled": true, "config": {} }
  ],
  "aliases": { "bash_over_ssh.bash": "ssh_bash" }
}
```

- Entries are keyed by **bare filename**. To toggle a plugin, set its
  `enabled` field (`true`/`false`); the file is rewritten on every startup, so
  edits persist.
- **Default state is by provenance:** bundled plugins default **enabled**,
  user plugins default **disabled**. One exception: `session-titler` ships
  disabled (hardcoded, since it spends tokens). Once an entry exists in
  `plugins.json`, your `enabled` value wins forever.
- **`config`** is a per-plugin JSON object forwarded verbatim to the plugin
  (e.g. the `logo` plugin's `text`/`styles`/`charset` options).
- A failed **bundled** plugin is a fatal startup error (losing `tools-fs` or
  the provider is a useless harness); a failed **user** plugin only warns.
- To replace a bundled plugin with your own version of the same name: disable
  the bundled entry in `plugins.json` and drop yours in the user dir (two
  registrations with one name are fatal).

## Writing a third-party plugin

A plugin is a module that links the shared core (`minimal::server_core` or
`minimal::client_core` — one copy in memory, ordinary symbol resolution) and
exports C symbols. Symbol *presence* is the capability declaration — there is
no manifest.

**Required (both hosts):** `minimal_plugin_abi_tag()` /
`minimal_client_plugin_abi_tag()`, `minimal_plugin_name()` (client twin), and
`minimal_plugin_version()` (client twin).

**Optional server registration hooks** (`src/server/plugins/include/minimal/plugins/plugin_abi.h`):

- `minimal_plugin_register_tools` — tools (with `tools.*` settings and
  per-plugin `config`)
- `minimal_plugin_register_provider_factories` — new `backend` kinds for
  providers.json
- `minimal_plugin_register_session_observer` — fire-and-forget session events
- `minimal_plugin_register_commands` — `/`-commands
- `minimal_plugin_register_services` — cross-plugin pull services
  (`ServiceRegistry`)
- `minimal_plugin_register_mention_resolver` — `@`-mentions (exclusive)
- `minimal_plugin_register_auth_provider` — (exclusive)
- `minimal_plugin_register_permission_advisor` — auto-answers `Ask` decisions
- `minimal_plugin_register_model_catalogs` — lazy model discovery
- `minimal_plugin_register_compaction_strategies`
- `minimal_plugin_register_hook_points` — new veto-capable hook events

**Optional client registration hooks** (`src/client/plugins/include/minimal/client/plugins/client_plugin_abi.h`):
views, start views, per-tool renderers, message renderers, status-bar
segments, syntax grammars, chat surface (exclusive), markdown engine
(exclusive).

**Generic RPC channel:** a server plugin can `Provide` an endpoint named
`<name>.rpc`, callable from any client via `PluginCall`/`PluginSubscribe`
(the `cost-view` client plugin consumes `cost-tracker.rpc` this way).

**Plugin storage:** per-plugin namespaced KV store (SQLite, under the data
dir) plus a per-plugin scratch directory `<data>/plugins/<plugin_name>/`.

Full API details and design notes: `docs/EXTENSIBILITY.md`. Test fakes in
`tests/support/` are a build target and exported for plugin authors.

## Aliases

Aliases live in the `"aliases"` map of `plugins.json` (client: the same map in
`client-plugins.json`) and **rename what the model and the user see**:

```json
{ "aliases": { "bash_over_ssh.bash": "ssh_bash" } }
```

- Keys are fully-qualified **`plugin.original_name`**; values are the surface
  name. One flat namespace covers **tools and commands together** (the old
  `command_aliases` key is rejected with a warning).
- Aliasing is the designed way to **resolve name collisions** — duplicate
  registrations are fatal, but applying an alias before registration makes
  both coexist (the canonical example: two plugins both providing `bash`).
- **Permission keys are never aliased.** `per_tool_overrides`,
  `allowed_tools`, and Ask-disclosures always use the *original* name, no
  matter what surface name the model sees.
- Dangling alias keys (matching nothing) produce startup warnings.
- **No aliases exist for models** (always `<provider>/<model>`) or agent
  presets (always the `agents/<name>.md` file stem).
