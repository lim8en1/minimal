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
| `tool-ask` | Tool `ask_user` — structured model→operator questions (up to 4, options with descriptions, multi-select, optional free text), answered through a client modal; see ARCHITECTURE.md "Asking the user" |
| `tools-bash` | Tool `bash`; commands `/restart-shell`, `/tasks`; hook point `before_command`; per-session cwd/env retention |
| `tools-web` | Tools `web_fetch`, `web_search` |
| `provider-openai` | Provider kind `openai` (chat-completions shape) + model catalog for it |
| `provider-anthropic` | Provider kind `anthropic` (Messages API, prompt caching) |
| `commands-core` | Commands `/help`, `/clear`, `/compact`, `/agent`, `/model`, `/sessions`, `/connect` (stores a provider credential in `<data dir>/auth.json`) |
| `commands-md` | Turns `commands/*.md` files into `/`-commands (config dir, project, and `.opencode/commands/`; opencode-syntax desugaring — see CONFIGURATION.md) |
| `checklist` | Tool `checklist` + cross-plugin RPC endpoint `checklist.rpc` |
| `signals-message` | Tool `message_session` (cross-session messaging on the core `signals` service) |
| `cost-tracker` | Commands `/cost`, `/cost-update-rates`; per-session cost accumulation; RPC endpoint `cost-tracker.rpc` |
| `tools-windbg` | Tools `windbg`, `windbg_interrupt` — debugger commands against a remote **dbgd** server (dbg-cli). See [tools-windbg](#tools-windbg) below |
| `session-titler` | Session observer that auto-titles sessions (~100 tokens/session spend). **Ships disabled** |

## Bundled client plugins (`plugins/client/`)

Installed to `<libdir>/minimal/plugins/client/`.

| Plugin | Provides |
|---|---|
| `logo` | The ASCII-banner start view (random banner per session) |
| `cost-view` | The `{cost}` status-bar segment (polls `cost-tracker.rpc`, renders cumulative $ / tokens) |
| `checklist-view` | The `{todo}` status-bar segment (polls `checklist.rpc` for the focused session) |

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

The pattern ("per-preset tool config", the `allowed_skills` delivery shape):

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
   how it combines with its own `plugins.json` `config` object (tools-python
   merges per-key, preset wins). Bundled example: `tools-python` keys the
   declared package contract under `python`.

**Optional client registration hooks** (`src/client/plugins/include/minimal/client/plugins/client_plugin_abi.h`):
views, start views, per-tool renderers, message renderers, status-bar
segments, syntax grammars, chat surface (exclusive), markdown engine
(exclusive).

**Generic RPC channel:** the generic `PluginCall`/`PluginSubscribe` RPCs dispatch by plugin name through the `ServiceRegistry`, under two suffixed conventions that cannot collide:

- **`<name>.rpc`** — a `PluginRpcHandler` (`plugin_rpc.hpp`): unary request/response. `checklist` and `cost-tracker` ship `.rpc` handlers (the `cost-view` client plugin polls `cost-tracker.rpc` this way).
- **`<name>.endpoint`** — a `PluginEndpoint` (`plugin_abi.h`): unary `Call` plus a `Subscribe` half that pushes to the subscriber until it disconnects. `PluginSubscribe` dispatches to `.endpoint` providers only; no bundled plugin ships one yet.

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
| `minimal/sidecar/lifetime_watch.hpp` | `WatchLifetime`: fires a cleanup once when the session's lifetime expires (the mechanism for resources that live OUTSIDE this process) |

The non-blocking contract a tool gets by combining the last two with the
deferred-completion handles on `ToolContext` (`emit_output`,
`schedule_injection`, `emit_task_event`, `session_lifetime`): the tool call
acknowledges immediately, output streams to the operator live, the result
reaches the model as an injected message at the next turn boundary, session
death terminates the work, and cancelling the turn that started it does not.
`tools-bash` (background tasks) and `tools-windbg` (remote debugger jobs)
are the worked examples.

### tools-windbg

The `windbg`/`windbg_interrupt` tools are a **client of a dbgd server**
(dbg-cli): the debugger engine — DbgEng via pybag, with dbg-cli's worker
isolation and dbgsrv/KDNET remoting — lives in the dbgd process on the
debugger host; this plugin only speaks its HTTP/JSON protocol
(`/v1/sessions`, exec with the ack-within-grace → job pattern, job
long-poll, the lease-free interrupt, `DELETE` on session end). Fast commands
answer inside the grace window; slow ones become background tasks (above)
whose watcher streams the job transcript and injects the verdict.

Configure via the plugin's `config` object in `plugins.json`:

```json
{ "file": "tools-windbg.so", "enabled": true,
  "config": {
    "base_url": "http://debugger-host:8477",
    "bearer_token": "<token from dbgd configure>",
    "default_target": { "kind": "exec", "server": "tcp:server=10.0.0.9,port=5555" }
  } }
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
