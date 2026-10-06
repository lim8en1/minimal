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

Installed to `<libdir>/minimal/plugins/server/`. All enabled by default unless noted.

| Plugin | Provides |
|---|---|
| `tools-fs` | Tools `read`, `write`, `edit` |
| `tools-search` | Tools `grep`, `glob` (RE2-backed, linear-time regex) |
| `tool-ask` | Tool `ask_user` — structured model→operator questions (up to 4, options with descriptions, multi-select, optional free text), asked through core's plugin prompt (kind `ask`) and answered in the `ask-view` client plugin's modal; see ARCHITECTURE.md "Asking the user" |
| `tools-bash` | Tool `bash`; commands `/restart-shell`, `/tasks`; hook point `before_command`; per-session cwd/env retention |
| `tools-web` | Tools `web_fetch`, `web_search`; a per-host permission model for them (`plugins.tools-web` / `plugin_permissions.tools-web`, see its README) |
| `mcp` | The MCP client: `mcps.json` servers as tool source `mcp` (`mcp__<server>__<tool>` tools), their per-server permission model, `/mcp`, the `mcp.endpoint` the `mcp-view` picker reads, and `channels.json` channels as external signal channels. Limits in its `config` (`connect_timeout_ms`, `request_timeout_ms`, `max_list_pages`, `restart_cooldown_ms`). Disabled, nothing MCP remains |
| `provider-openai` | Provider kind `openai` (chat-completions shape) + model catalog for it |
| `provider-anthropic` | Provider kind `anthropic` (Messages API, prompt caching) |
| `commands-core` | Commands `/help`, `/clear`, `/compact`, `/agent`, `/model`, `/sessions`, `/secret` (named secrets in `<data dir>/secrets.json`, referenced as `secret:<name>`) — `/mcp` is the `mcp` plugin's |
| `commands-md` | Turns `commands/*.md` files into `/`-commands (config dir, project, and `.opencode/commands/`; opencode-syntax desugaring — see CONFIGURATION.md, how-to in COMMANDS_HOWTO.md) |
| `checklist` | Tool `checklist` + cross-plugin RPC endpoint `checklist.rpc`, and the `checklist.endpoint` whose push stream `checklist-view` subscribes to |
| `signals-message` | Tool `message_session` (cross-session messaging on the core `signals` service) |
| `cost-tracker` | Command `/cost`; startup auto-refresh of pricing snapshots (also `--plugin-call cost-tracker refresh_rates`); per-session cost accumulation; RPC endpoint `cost-tracker.rpc`, and the `cost-tracker.endpoint` whose totals push stream `cost-view` subscribes to |
| `session-titler` | Session observer that auto-titles sessions (~100 tokens/session spend). **Ships disabled** |
| `skills` | `SKILL.md` instruction bundles: the `skill` tool, its per-skill permission model (a preset's `plugin_permissions.skills`), the per-preset listing in the tool description and preloaded bodies in the system prompt, and the project's skill dirs in the trust prompt. Disabled, no skill concept remains |
| `project-context` | The project's instructions file (`AGENTS.md`, else `CLAUDE.md`) as the first section of every session's context (context contributor, priority 0). Config: `{"files": [...], "max_bytes": 262144, "search_ancestors": false}`. Disabled, no instructions file is read |
| `compaction-structured` | The `structured` compaction strategy: the summarizer answers through a strict `record_summary` tool call parsed into typed sections (task, done, next steps, topics, important, lessons, future tasks). Its instruction is `prompts/compression-structured.md`. Disabled, a setting or preset naming `structured` warns and falls back to core's `summarize` |
| `opencode-compat` | Reads opencode's config so an existing setup works zero-copy: opencode-shaped `providers.json` entries (`npm`, `options.baseURL/apiKey/headers`, `models.<id>.limit/options/name`) are translated into core's shape, and opencode's `~/.local/share/opencode/auth.json` is the `secret:opencode-compat:<provider-id>` secret namespace. Disabled, core reads only its own shapes |

## Bundled client plugins (`plugins/client/`)

Installed to `<libdir>/minimal/plugins/client/`.

| Plugin | Provides |
|---|---|
| `logo` | The ASCII-banner start view (random banner per session) |
| `cost-view` | The `{cost}` and `{token_detail}` status-bar segments (subscribe to `cost-tracker.endpoint`'s totals stream, polling only as a fallback; render cumulative $ and the token breakdown) |
| `ask-view` | The client half of `tool-ask`: the `ask` prompt renderer (the question modal) and the `ask` display renderer (the transcript row, expanded by default). Disabled, every ask is answered "unavailable" |
| `config-view` | `/config [plugin]`: the `config` modal view over core's `plugins` endpoint — the configurable plugins, then a plugin's schema form (values, defaults, live/restart, restart pending), edited in place and validated by the plugin. The form is client core's reusable `client::MakeSchemaForm` |
| `mcp-view` | The `/mcp` picker: the `mcp` modal view (`view_hint` "mcp") over the `mcp` plugin's `mcp.endpoint` — lists servers with live status, Enter toggles one |
| `checklist-view` | The `{todo}` status-bar segment + the `checklist.panel` view (both fed by one shared worker holding `checklist.endpoint`'s change stream, polling only as a fallback; the panel toggles between a focused-tab tasklist and a per-tab summary; default binding `alt+l`) |

## User plugins (`user-plugins/`)

Plugins that live outside this repo — your own, or third-party ones — are
developed against the harness build itself and deploy into the config dir.
Because the ABI tag pins plugins to the exact harness tree (source-compatible,
not binary-compatible), the plugin sources are *built by* the harness build:
`make build` compiles them with the same compiler, flags, and ABI tag, then
deploys them. There is no SDK to install and nothing to re-point after a pull.

### Registering a plugin

Each plugin is one directory under `user-plugins/server/` or
`user-plugins/client/` (both gitignored). The subtree is the plugin's **kind**;
a plugin's `CMakeLists.txt` is the same one-liner on either side:

```cmake
minimal_add_user_plugin(my-plugin plugin.cpp)
```

To park an entry without unlinking it (an unmaintained plugin whose sources
lag the harness), create an empty `user-plugins/<kind>/<name>.skip` beside it:
the entry is not configured or built until the file is removed.

Entries can be:

- a **symlink** into your own plugin repo — how a repo holding both server and
  client plugins registers (one link per plugin, in the matching subtree).
  `tools-windbg` is the worked example:

  ```sh
  scripts/new-plugin.sh --link ../minimal-for-vulnerability-research/server/tools-windbg
  ```

- a **git clone** of a third-party plugin repo (cloned directly into the tree;
  works identically),
- a **real directory** — what the scaffold creates.

New entries are picked up by the next `make build` (the glob re-runs on
configure).

### Creating one

```sh
scripts/new-plugin.sh my-plugin            # server; add --client for the client side
scripts/new-plugin.sh --link ../my-plugins/server/my-plugin
```

The scaffold writes `user-plugins/server/my-plugin/{CMakeLists.txt, plugin.cpp}`
— a working minimal plugin (one tool, or one status segment) with commented
pointers to every other registration hook. In-repo sources are unversioned
(the directory is gitignored); move a plugin into its own repo and re-register
with `--link` once it earns its keep.

### Deploy and enablement

Every `make build` deploys rebuilt user plugins into `<config>/plugins/`
(server) or `<config>/client-plugins/` (client) — an atomic copy+rename, so a
rebuild cannot truncate the artifact under a running server or client. The
deploy **overwrites the previous build but never prunes**: the user dir is also
the by-hand drop location, so retiring a plugin means deleting its `.so` there
yourself.

User plugins default **disabled** (a trust boundary, not friction): run the
server once so discovery records the entry, then set `"enabled": true` in
`plugins.json` / `client-plugins.json`. A failed user plugin warns instead of
aborting startup, and the basename-collision rule is the designed way to
replace a bundled plugin with your own version of the same name (see
"Discovery, enablement, replacement" below).

The deploy destination resolves at configure time, in the same order the
server resolves the config dir: `MINIMAL_CONFIG_DIR` → `XDG_CONFIG_HOME/minimal`
→ `~/.config/minimal`. Override it (e.g. for CI) with the CMake cache variable
`MINIMAL_USER_PLUGIN_DEPLOY_DIR`.

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
    { "file": "tools-fs.so", "provenance": "bundled", "enabled": true }
  ],
  "aliases": { "bash_over_ssh.bash": "ssh_bash" }
}
```

- Entries are keyed by **bare filename**. To toggle a plugin, set its
  `enabled` field (`true`/`false`); the file is rewritten on every startup, so
  edits persist.
- **Default state is by provenance:** bundled plugins default **enabled**,
  user plugins default **disabled**. A bundled plugin can opt out of the
  enabled default from its own build: `minimal_add_plugin(<name>
  DISABLED_BY_DEFAULT ...)` installs a `<name>.disabled-by-default` marker
  beside the module (`session-titler` and `decision-advisor` do). Once an
  entry says `enabled`, your value wins forever.
- A hand-written entry may omit `provenance` (the file name picks the
  plugin; an error when both directories ship that name) and `enabled` (the
  default above). Both are written back once resolved.
- **A plugin's own settings** live in their own file, one per plugin:
  `<config>/plugins.d/<stem>.json` (client: `<config>/client-plugins.d/`),
  where `<stem>` is the module's file name without its extension — the
  plugin's name for every bundled plugin. The object is forwarded verbatim
  (e.g. `client-plugins.d/logo.json` holds the `logo` plugin's
  `text`/`styles`/`charset` options); no file means `{}`. Only enabled
  plugins' files are read. A file that is not a JSON object fails its plugin,
  naming the file. A leftover `config` key on a `plugins.json` entry is a
  startup error naming the file it moved to.
- Every bundled plugin parses its file strictly: an unknown field or a bad
  value is a startup error naming `plugins.d/<plugin>.json: <field>`. A
  third-party plugin gets the same behaviour from the SDK's `config::Reader`
  (`minimal/common/config_reader.hpp`), labelled with
  `common::PluginConfigLabel` (`minimal/common/plugin_config_label.hpp`;
  further files of its own through `minimal/common/json_config_loader.hpp`),
  which names the file and full path in every error and treats `"//"` keys as
  comments.
- **The file is strict.** Invalid JSON, or any entry with a wrong type or an
  unknown field, is fatal on the server (no plugin loads, the file is not
  rewritten) and a warning on the client (that entry is skipped, the file is
  not rewritten). Degrading to defaults would silently re-enable a plugin
  you disabled.
- A failed **bundled** plugin is a fatal startup error (losing `tools-fs` or
  the provider is a useless harness); a failed **user** plugin only warns.
- To replace a bundled plugin with your own version of the same name: disable
  the bundled entry in `plugins.json` and drop yours in the user dir (two
  registrations with one name are fatal).

### Configuring from a shell: `minimal plugin`

```
minimal plugin                          # every plugin: enabled, configured or not
minimal plugin tools-web                # every field: value, default or set (*), apply, doc
minimal plugin get tools-web search.api_url
minimal plugin set tools-web search.api_url 'http://localhost:8888/search?format=json'
minimal plugin unset tools-web fetch.timeout_ms
minimal plugin tools-web --json         # schema, file contents, effective values
```

Keys are dotted paths into `plugins.d/<name>.json`. A value is parsed by
the field's type (integers, booleans and enums as such, strings verbatim,
lists and opaque fields as JSON) and then checked by the plugin's own parser
before the file is written, so a refused value never reaches the file:

```
$ minimal plugin set tools-web fetch.max_bytes 1
minimal: plugins.d/tools-web.json: fetch.max_bytes: must not be below fetch.default_max_bytes
```

`set` says how the change applies: **live** (every changed field is `live`:
a running server takes it without a restart) or **restart**. When a server
is running, `minimal plugin set`/`unset` then asks it to apply the change
(the `plugins` endpoint's `apply`) and reports the outcome; a restart change
stays listed as pending there until the server restarts. Unsetting the last
key removes the file. The work runs in `minimal-server plugin ...`,
which opens plugins for their config symbols only — no registration, no
storage — so it is safe beside a running server. A secret field's literal
value is shown as `****`; a `secret:`/`env:` reference is shown as written.

## Writing a third-party plugin

See ["User plugins"](#user-plugins-user-plugins) for the build-and-deploy
workflow (`user-plugins/`, `scripts/new-plugin.sh`); this section is the API
itself.

A plugin is a module that links the shared core (`minimal::server_core` or
`minimal::client_core` — one copy in memory, ordinary symbol resolution) and
exports C symbols. Symbol *presence* is the capability declaration — there is
no manifest.

**Required (both hosts):** `minimal_plugin_abi_tag()` /
`minimal_client_plugin_abi_tag()`, `minimal_plugin_name()` (client twin), and
`minimal_plugin_version()` (client twin). Define these with the
`MINIMAL_PLUGIN_ABI_SYMBOLS(plugin_name_literal)` macro (server) /
`MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS(plugin_name_literal)` macro (client) from
`plugin_abi.h`, invoked once at file scope — not by hand. A handwritten
`return minimal::common::AbiTag();` compiles and looks right, but
`minimal_common` is a static library re-exported by the shared core, so the
call binds at `dlopen` time to the *host's* copy, not anything baked into the
plugin: the "you forgot to rebuild" guard then compares the host against
itself and always passes, even for a stale plugin. The macro instead bakes
the compile-time `MINIMAL_ABI_TAG`/`MINIMAL_VERSION` into the plugin's own
object code.

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
- `minimal_plugin_register_context_contributors` — sections pre-filled into
  each session's system context (additive; asked once per session, frozen
  for it; see `minimal/agent/context_contributor.hpp`)
- `minimal_plugin_register_auth_provider` — (exclusive)
- `minimal_plugin_register_permission_advisor` — auto-answers `Ask` decisions
- `minimal_plugin_register_permission_models` — the permission model for the
  plugin's OWN tools (one per plugin; `minimal/tools/permission_model.hpp`):
  claims them, replaces core's base and/or network stage for them, reads its
  slices from `permissions.json` `plugins.<plugin>` and preset
  `plugin_permissions.<plugin>`. Core still folds per-tool denies, ceilings,
  run modes, grants and the prompt over its answer
- `minimal_plugin_register_tool_sources` — dynamic tools a preset allows by
  member (`tool_sources: {<source>: [members]}`); pair with
  `ToolRegistry::ReserveNamespace` + `ReplaceGroupTools` in `register_tools`
  (`minimal/tools/tool_source.hpp`)
- `minimal_plugin_shutdown` — stop threads/processes after every session
  ended (plugins are never unloaded)
- `minimal_plugin_register_model_catalogs` — lazy model discovery
- `minimal_plugin_register_compaction_strategies`
- `minimal_plugin_register_hook_points` — new veto-capable hook events
- `minimal_plugin_config_schema` / `minimal_plugin_validate_config` — the
  plugin's `plugins.d` schema and a dry-run check, both from its own parser:
  one `MINIMAL_PLUGIN_CONFIG_SCHEMA(label, parse)` line
  (`minimal/common/plugin_config_schema.hpp`). Called with no registration
  context (the `minimal plugin` CLI, the config view), so the parser must
  be side-effect free. Describe mode records every field the parser reads
  through `config::Reader` (path, type, default, range, choices);
  `reader.Describe(key, {doc, apply, secret})` adds the rest, and
  `reader.RelevantWhen(key, field, {values})` / `RelevantWhenSet(key, field)`
  say a field matters only in some modes (memory's http fields under
  `strategy` http/auto). The CLI and `/config` show an unused field as such,
  and changing one has no effect on the running plugin
- `minimal_plugin_reconfigure` — take a changed config without a restart.
  Only fields declared `ApplyPolicy::kLive` change this way; the host calls
  it only when every changed field is live. Keep the settings in a
  `common::LiveValue<T>` (`minimal/common/live_value.hpp`) and read one
  snapshot per call. Keep configured values out of tool definitions: a
  definition sits in every cached prompt prefix, so quoting a limit there
  leaves the model a stale number after a live change and invalidates the
  cache on every edit. State the limit in the result where it applied
  instead (bash's timed-out result, web_fetch's truncation footer)

A plugin that owns a config file validates it itself and reports problems
through `PluginRegistrationContext::report_warning`; the startup sanity check
prints them under the plugin's name. Fail registration (throw) only for
something the plugin cannot run without.

**Per-preset tool config** — a tool can receive configuration from the active
agent preset:

```markdown
---
name: rev
tool_config:
  python:            # the namespace the tool declares
    packages: [impacket]
---
```

The pattern ("per-preset tool config"):

1. Declare the namespace: override `Tool::config_namespace()` (default empty =
   reads nothing). Several tools of ONE plugin may share a namespace; two
   plugins claiming one namespace is a **fatal** startup collision (it cannot
   be aliased), and an unclaimed namespace in any preset is a **warning** —
   both via the startup sanity check.
2. Read the slice per invocation: `ctx.tool_config` on `ToolContext` points at
   THIS tool's namespace slice (null when the preset declares none). It is
   resolved fresh each call from the active preset, so `/agent` switches take
   effect at the next tool call.
3. Own your schema and merge: core never interprets a slice; the plugin decides
   how it combines with its own `plugins.d` file (tools-python
   merges per-key, preset wins). Bundled example: `tools-python` keys the
   declared package contract under `python`.

**Optional client registration hooks** (`src/client/plugins/include/minimal/client/plugins/client_plugin_abi.h`):
views, start views, client-side /-commands (run in the TUI, no server round
trip; the client's own commands win a name collision), per-tool renderers,
display renderers, prompt renderers
(the modal for a server plugin's `prompt_user` kind), message renderers, status-bar
segments, syntax grammars, chat surface (exclusive), markdown engine
(exclusive).

**Generic RPC channel:** the generic `PluginCall`/`PluginSubscribe` RPCs dispatch by plugin name through the `ServiceRegistry`, under two suffixed conventions that cannot collide:

- **`<name>.rpc`** — a `PluginRpcHandler` (`plugin_rpc.hpp`): unary request/response. `checklist` and `cost-tracker` ship `.rpc` handlers for cross-plugin callers.
- **`<name>.endpoint`** — a `PluginEndpoint` (`plugin_abi.h`): unary `Call` plus a `Subscribe` half that pushes to the subscriber until it disconnects. `PluginSubscribe` dispatches to `.endpoint` providers only. `checklist`, `cost-tracker` and `mcp` ship one; their client views subscribe.

`PluginCall` tries `.endpoint` first, then falls back to `.rpc`.

**Plugin storage:** per-plugin namespaced KV store (SQLite, under the data
dir) plus a per-plugin scratch directory `<data>/plugins/<plugin_name>/`.

Full API details and design notes: `docs/EXTENSIBILITY.md`. The test fakes in
`tests/support/` (`minimal::test_support`) are an in-tree INTERFACE target for
this repo's tests only — not installed or exported to plugin authors.

## The sidecar core (`minimal/sidecar/`)

`src/server/sidecar/` is the shared machinery behind every non-blocking
tool — the parts tools-bash and tools-python used to each carry a private
copy of, now one implementation in the shared core. New tools should build
on it instead of forking process code again:

| Header | What it gives you |
|---|---|
| `minimal/sidecar/process.hpp` | `fork`+`execve` launcher with pipe plans (which pipe end lands on which child fd, incl. `>2` passthrough fds), rlimits, `setsid` + `killpg`, the process-global pgid set with the atexit sweep |
| `minimal/sidecar/drain.hpp` | `DrainAndReap`: sliced poll of the launched pipes into per-pipe sinks (`kCapped` head+tail with an optional live emit tap, `kBounded` all-or-nothing accumulate, `kDiscard`), deadline + cancellation, bounded post-kill reap |
| `minimal/sidecar/capped_buffer.hpp` | The head+tail capped output buffer |
| `minimal/sidecar/task_registry.hpp` | The generic background-task registry: id minting, per-session records/limits, `STARTED`-before-watch ordering, lifecycle events, result injection at a turn boundary, terminal-state-stored-last quiescence, watcher containment |

The non-blocking contract a tool gets by combining the task registry with the
deferred-completion handles on `ToolContext` (`emit_output`,
`schedule_injection`, `emit_task_event`, `session_lifetime`): the tool call
acknowledges immediately, output streams to the operator live, the result
reaches the model as an injected message at the next turn boundary, session
death terminates the work, and cancelling the turn that started it does not.
`tools-bash` (background tasks) is the worked example. A resource that lives
outside this process is torn down the same way: the watcher thread that owns
it polls `session_lifetime`, and `SessionObserver::OnSessionEnded` releases the
per-session bookkeeping.

### tools-windbg (user plugin)

The `windbg`/`windbg_interrupt` tools are a **client of a dbgd server**
(dbg-cli): the debugger engine — DbgEng via pybag, with dbg-cli's worker
isolation and dbgsrv/KDNET remoting — lives in the dbgd process on the
debugger host; the plugin only speaks its HTTP/JSON protocol
(`/v1/sessions`, exec with the ack-within-grace → job pattern, job
long-poll, the lease-free interrupt, `DELETE` on session end). Fast commands
answer inside the grace window; slow ones become background tasks (above)
whose watcher streams the job transcript and injects the verdict.

It used to be bundled; it now lives in its own user plugin repo, registered
through the `user-plugins/` tree (see "User plugins" above) — the reference
example of the user plugin workflow, tests included. Enable its
`plugins.json` entry once discovery records it (provenance `user`, so it
starts disabled), and configure it in `plugins.d/tools-windbg.json`:

```json
{
  "base_url": "http://debugger-host:8477",
  "bearer_token": "<token from dbgd configure>",
  "default_target": { "kind": "exec", "server": "tcp:server=10.0.0.9,port=5555" }
}
```

`default_target` is a dbgd `TargetSpec` passed through verbatim (`exec`, `pid`,
`dump`, or `kernel`); a call can also supply its own `target` object, which
wins for that first connect. Closing a session on harness-session end uses
the `on_session_end` disposition (`detach`, the default, or `kill`).

## Aliases

Aliases live in the `"aliases"` map of `plugins.json` (client: the same map in
`client-plugins.json`) and **rename what the model and the user see**:

```json
{ "aliases": { "bash_over_ssh.bash": "ssh_bash" } }
```

- Only **names** are aliasable — keys a plugin chooses and a user or the
  model addresses: tools and commands (server), view ids and status segments
  (client). The renamed entry answers to its alias everywhere it is
  addressed: the model calls the tool by it, the user types the command,
  `keybindings.json` binds the view, `ui.json`'s `status_format` places the
  segment, a command's view hint opens it. A plugin's own code keeps using
  its original id.
- **Slots are never aliased** — keys that are a contract with something
  outside the plugin: service names, hook points, renderer payload kinds, the
  tool a result renderer draws, grammar languages, client commands. Renaming
  one would only detach it from what it serves, so an alias naming a slot is
  a startup warning and a slot collision is resolved by disabling a plugin.
- Keys are fully-qualified **`plugin.original_name`**; values are the surface
  name. A plugin providing a tool and a command under one name renames both.
- Aliasing is the designed way to **resolve name collisions** — duplicate
  registrations are fatal, but applying an alias before registration makes
  both coexist (the canonical example: two plugins both providing `bash`).
- **Permission keys are never aliased.** `per_tool_overrides`,
  `allowed_tools`, and Ask-disclosures always use the *original* name, no
  matter what surface name the model sees.
- Dangling alias keys (matching nothing) produce startup warnings.
- **No aliases exist for models** (always `<provider>/<model>`) or agent
  presets (always the `agents/<name>.md` file stem).
