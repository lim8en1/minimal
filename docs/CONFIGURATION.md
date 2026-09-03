# Configuration reference

Everything minimal reads at startup lives in one config directory, resolved in
this order (both server and client):

1. `$MINIMAL_CONFIG_DIR` — explicit override
2. `$XDG_CONFIG_HOME/minimal`
3. `$HOME/.config/minimal` — default (not the macOS `~/Library` path)

Runtime state (SQLite session store, logs, plugin KV storage) lives in a
separate data dir: `$XDG_DATA_HOME/minimal`, else `$HOME/.local/share/minimal`.

The config dir is seeded from `bundled/` at `cmake --install` time (see
BUILD.md); existing files are never overwritten. Nothing is seeded at server
runtime.

## File inventory

| Path | Process | Status | If missing / malformed |
|---|---|---|---|
| `settings.json` | server | **required** | missing or malformed → **fatal** |
| `providers.json` | server | **required** | malformed → **fatal**; `{}` is valid but "no usable model" is fatal |
| `permissions.json` | server | **required** | malformed → **fatal** |
| `hooks.json` | server | optional | missing → no hooks; empty file treated as absent; malformed → **fatal** |
| `mcps.json` | server | optional | missing → no MCP; malformed entries → warning, degrade to no MCP |
| `channels.json` | server | optional | same degrade-to-warning behavior as `mcps.json` |
| `plugins.json` | server | optional | malformed → warning, treated as empty (see docs/PLUGINS.md) |
| `agents/<name>.md` | server | **at least one required** | none resolvable → **fatal** |
| `prompts/<name>.md` | server | optional | missing `prompts/compression.md` → warning |
| `cost-rates.json` | server | optional | cost-tracker's fallback price table |
| `plugins/` (dir) | server | optional | user plugins scanned here (docs/PLUGINS.md) |
| `client-plugins.json` | client | optional | malformed → warning, treated as empty |
| `client-plugins/` (dir) | client | optional | user client plugins scanned here |
| `keybindings.json` | client | optional | missing → built-in defaults; malformed → load fails (named path) |
| `ui.json` | client | optional | malformed → warning + all defaults |
| `themes/<name>.json` | client | optional | unknown theme name → terminal defaults + warning |

Startup errors are gathered by a sanity check: **fatal** problems abort the
server; **warnings** are logged and startup continues. Unless noted otherwise
below, unknown fields are silently ignored and wrong-typed or out-of-range
values are fatal (the file is refused, not patched).

---

## settings.json

Full example (`bundled/settings.json`):

```json
{
  "default_agent_preset": "build",
  "default_model": "local/your-model-name",
  "max_concurrent_subagents": 4,
  "background_wake": false,
  "memory": {
    "auto_compact": true,
    "context_compaction_trigger": { "percent": 80 },
    "scrollback_page_size": 100
  },
  "tools": {},
  "tools_view": { "stdout_buffer_lines": 100 },
  "project_context": {
    "enabled": true,
    "files": ["AGENTS.md", "CLAUDE.md"],
    "max_bytes": 0,
    "search_ancestors": false
  },
  "logging": { "level": "info", "max_size_mb": 10, "max_files": 5 }
}
```

### Top level

| Field | Type | Default | Notes |
|---|---|---|---|
| `default_agent_preset` | string | `""` | file stem of an `agents/<name>.md` preset; unresolvable value → **fatal** |
| `default_model` | string | `""` | `"<provider>/<model>"` (see providers.json); a typo → warning at startup, first use fails |
| `default_theme` | string | `""` | client theme name (`themes/<name>.json`) |
| `max_concurrent_subagents` | int | `4` | `<= 0` or `> 8` → warning |
| `background_wake` | bool | `false` | |
| `session_db_path` | string | `""` | empty → resolved under the data dir |

### `memory.*`

| Field | Type | Default | Notes |
|---|---|---|---|
| `auto_compact` | bool | `true` | |
| `context_compaction_trigger` | object | absent = never | **exactly one** of `percent` (1–100), `tokens` (> 0), `buffer_tokens` (> 0) |
| `context_compaction_trigger_tokens` | int | `0` | legacy flat key, honored only when the object form is absent; `0` = never |
| `compaction_keep_recent_tokens` | int | `0` | |
| `compaction_strategy` | string | `"summarize"` | name of a registered strategy (core built-in or plugin-registered); unknown name → warning, falls back |
| `scrollback_page_size` | int | `0` | |

### `tools.*`

`tools` is also passed **verbatim to plugins** — unknown subkeys are legal and
plugin-owned (e.g. per-tool settings for `read`/`edit`/`bash`). Core recognizes:

| Field | Type | Default | Notes |
|---|---|---|---|
| `read`, `edit`, `bash` | object | `{}` | raw JSON, schema owned by the respective tool plugin |
| `mcp.connect_timeout_ms` | int | `10000` | |
| `mcp.request_timeout_ms` | int | `60000` | |
| `mcp.max_list_pages` | int | `100` | must be ≥ 1 |
| `mcp.restart_cooldown_ms` | int | `30000` | must be ≥ 0 |
| `max_parallel_tool_calls` | int | `4` | must be ≥ 1; `1` = inline execution, no pool threads |
| `max_input_bytes` | int | `10485760` | ≥ 0; `0` disables the limit |
| `max_result_bytes` | int | `10485760` | ≥ 0; `0` disables the limit |

### `tools_view.*`

| Field | Type | Default |
|---|---|---|
| `stdout_buffer_lines` | int | `100` |

### `project_context.*`

| Field | Type | Default | Notes |
|---|---|---|---|
| `enabled` | bool | `true` | |
| `files` | string array | `["AGENTS.md", "CLAUDE.md"]` | plain file names only — no `/` or `\`, non-empty |
| `max_bytes` | int | `0` | `0` = unlimited |
| `search_ancestors` | bool | `false` | walk up from the project root |

### `logging.*`

| Field | Type | Default | Allowed values |
|---|---|---|---|
| `level` | string | `"info"` | `trace` \| `debug` \| `info` \| `warn` \| `warning` \| `error` |
| `path` | string | `""` | |
| `max_size_mb` | int | `10` | |
| `max_files` | int | `5` | |

### `sessions.*` and `server.*`

All values must be ≥ 0 (negative → **fatal**); `0` means "never" where noted.

| Field | Type | Default | Notes |
|---|---|---|---|
| `sessions.detached_idle_ttl_seconds` | int | `1800` | idle reaper collects a fully quiescent detached session after this; `0` = never |
| `sessions.unattended_prompt_timeout_seconds` | int | `0` | bound on permission/confirmation waits for detached sessions; `0` = wait forever |
| `sessions.initial_attach_guard_seconds` | int | `10` | guard for sessions that never attach a client |
| `server.idle_shutdown_seconds` | int | `5` | exit after this long with zero attached clients and zero live sessions; `0` = never (daemon) |

---

## providers.json

```json
{
  "providers": {
    "local": {
      "backend": "openai",
      "url": "http://localhost:8000/v1",
      "max_tokens": 4096,
      "parameters": { "temperature": 0.3 },
      "models": {
        "qwen4": { "context_window_tokens": 131072 }
      }
    }
  }
}
```

Top level is exactly `{"providers": { "<name>": {...} }}` — the key is the
provider name (no `/` allowed, in provider or model names). Provider and model
names are addressed everywhere as `"<provider>/<model>"`. `{}` is valid (but
then no model is usable). Unknown fields ignored. `bundled/providers.example.json`
is a commented starting point.

### Per provider

| Field | Required | Type | Default | Notes |
|---|---|---|---|---|
| `backend` | **yes** | string | — | a provider *kind*: `"openai"`, `"anthropic"` (bundled) or anything a plugin registered. Unknown kind → warning |
| `url` | **yes** | string | — | `openai` backend: includes `/v1`. `anthropic` backend: **without** `/v1` — the provider appends `/v1/messages` itself |
| `api_key` | no | string | — | inline key. Mutually exclusive with `api_key_env`; prefer `api_key_env` |
| `api_key_env` | no | string | — | name of an env var read at startup; naming an unset var → error. Mutually exclusive with `api_key` |
| `discover` | no | bool | `false` | `true`: models are listed lazily from the backend (`GET <url>/models`) instead of a `models` object; never written back; failures fall back to any static entries; `/providers refresh` re-fetches |
| `models` | yes (unless `discover`) | object | — | keyed by model name |
| `parameters` | no | object | `{}` | arbitrary sampling/generation knobs passed through to the backend (e.g. `temperature`, `top_p`, `cache_ttl` for anthropic: `"5m"` or `"1h"`). Model-level `parameters` **replaces** (does not merge) the provider's |
| `max_tokens` | no | int | — | required for the `anthropic` backend (here or per-model) or every request fails |
| `context_window_tokens` | no | int | — | per provider = default for all its models |

### Per model (`models.<name>`)

| Field | Required | Notes |
|---|---|---|
| `context_window_tokens` | no | the one field whose staleness disables compaction — set it. Model value wins over the provider default |
| `max_tokens` | no | inherits from provider when absent |
| `parameters` | no | **replaces** the provider's `parameters` when present |

The model *key* is sent on the wire verbatim as the request's `model` field —
it must be the backend's real model id, unlike the provider name which is
yours to choose.

---

## permissions.json

```json
{
  "default_run_mode": "normal",
  "fallback": {
    "read_only": "allow",
    "file_write": "ask",
    "execute": "ask",
    "out_of_workdir": "ask",
    "network": "ask"
  },
  "per_tool_overrides": {},
  "mcp_default_mode": "ask"
}
```

Mode vocabulary:

- **Permission mode** (what a rule assigns): `"allow"` | `"ask"` | `"deny"`
- **Run mode** (how the session treats the substrate): `"normal"` | `"bypass"` | `"allow-only"`

| Field | Type | Default | Notes |
|---|---|---|---|
| `default_run_mode` | string | `"normal"` | one of the run modes |
| `fallback` | object | harness defaults | five categories, each a permission mode: `read_only`, `file_write`, `execute`, `out_of_workdir`, `network`. Unset categories keep the harness default (bundled: `read_only=allow`, rest `ask`) |
| `per_tool_overrides` | object | `{}` | tool permission key (bare tool name or `plugin.tool`) → permission mode. Keys that resolve to no registered tool → warning. Aliased tools keep their **original** name here (see docs/PLUGINS.md) |
| `mcp_default_mode` | string | `"ask"` | default permission mode for MCP tools |
| `advisor` | string | — | plugin name of a permission advisor; unknown name → warning |
| `advisor_inherit` | bool | `false` | |
| `advisor_timeout_ms` | int | `30000` | ≥ 0; `0` disables the advisor budget |

There are no glob/rule lists — only per-category fallbacks and per-tool
overrides.

---

## hooks.json

Hooks run shell commands at named hook points; `bundled/hooks.example.json` is
a commented template.

```json
{
  "default_timeout_ms": 10000,
  "hooks": [
    {
      "on": "core.pre_tool_use",
      "filter": { "tool_name": "bash" },
      "command": "~/bin/minimal-guards/check-bash.sh",
      "timeout_ms": 10000
    }
  ]
}
```

| Field | Required | Notes |
|---|---|---|
| `default_timeout_ms` | no | int, default `10000`; must be ≥ 0 (`0` = no budget) |
| `hooks` | no | array of actions |

Each action:

| Field | Required | Notes |
|---|---|---|
| `on` | **yes** | hook point name. Unknown/never-registered point → warning, not fatal |
| `command` | **yes** | run via `/bin/sh -c` with the JSON payload on stdin; never argv-interpolated |
| `timeout_ms` | no | int ≥ 0; default = `default_timeout_ms` |
| `filter` | no | object of string→string, exact-match, ANDed; only on fields the point declares filterable |

Core hook points (plugins may register more, e.g. `tools-bash.before_command`):

| Point | Filterable fields |
|---|---|
| `core.pre_tool_use` (veto-capable) | `tool_name`, `permission_key`, `category` |
| `core.post_tool_use` | `tool_name`, `permission_key`, `category` |
| `core.user_prompt_submit` | — |
| `core.stop` | `stop_reason` |
| `core.session_created`, `core.session_ended` | — |
| `core.pre_compact`, `core.post_compact` | `trigger` |
| `core.subagent_started` | — |
| `core.subagent_completed` | `stop_reason` |
| `core.permission_prompted` | `tool_name`, `permission_key`, `category` |

**Exit codes:** `0` = pass. `2` = block the emission (veto-capable points
only; reason = first non-empty output line). Anything else **fails open** and
logs — a broken hook never widens what the permission substrate allowed and
never wedges the session.

---

## mcps.json

Top-level **array** of MCP server entries. File-level error only if the file is
not an array; per-entry problems become warnings and the good entries survive.

| Field | Required | Notes |
|---|---|---|
| `name` | **yes** | non-empty, unique (duplicate → error for that entry) |
| `transport` | **yes** | `"stdio"` or `"http"` |
| `command` | stdio: **yes** | non-empty |
| `args` | no | array of strings (stdio) |
| `env` | no | string→string map (stdio); values may contain `${ENV_VAR}` markers, resolved at connection time |
| `url` | http: **yes** | non-empty |
| `headers` | no | string→string map (http); `${ENV_VAR}` interpolation applies |
| `enabled` | no | bool, default `true` |

Warnings at startup: stdio `command` not on PATH, http `url` not well-formed.

## channels.json

Top-level **object keyed by channel name** (external event ingress; stdio-only
in v1). Name must match `[a-z0-9._-]`; `"sessions"` is reserved; a name that
also appears in `mcps.json` → warning, channel skipped.

| Field | Required | Notes |
|---|---|---|
| `mode` | no | `"restricted"` (default) or `"extended"` |
| `signals` | restricted: **yes** | non-empty array of `[a-z0-9._-]` names. **Forbidden** on `extended` |
| `command` | **yes** | non-empty |
| `args` | no | array of strings |
| `env` | no | string→string map |
| `enabled` | no | bool, default `true` |

---

## Client configuration

### keybindings.json

Flat object mapping action name → key string (e.g. `"tab.next": "alt+down"`).
Missing file → built-in defaults; non-string value fails the load naming the
action. Unknown action names are **kept, not rejected** — plugin views
register runtime ids. Bundled actions (see `bundled/keybindings.json`):
`tab.next`, `tab.previous`, `group.previous`, `group.next`, `jump.attention`,
`session.picker`, `agent.picker`, `model.picker`, `help.overlay`,
`permission.cycle`, `panel.toggle`, `tab.close`, `chat.load_older`,
`chat.scroll_up`, `chat.scroll_down`, `chat.scroll_bottom`, `chat.row_up`,
`chat.row_down`, `chat.toggle_collapse`, `chat.toggle_collapse_all`,
`chat.toggle_timestamps`, `turn.cancel`, `app.quit`.

### ui.json

| Field | Type | Default | Notes |
|---|---|---|---|
| `status_format` | string | built-in default | `{version} \| {session} \| {activity} \| {tokens} \| {model} \| {agent} \| {permission_mode}` placeholders |
| `scrollback_window_max_messages` | int | `0` (= built-in) | must be positive if set |
| `tool_output_buffer_bytes` | int | `0` | |
| `tools_panel_stdout_buffer_lines` | int | `0` | distinct from the server's `tools_view.stdout_buffer_lines` |
| `theme` | string | `""` (= `"dark"`) | names `themes/<name>.json`; unknown name → terminal defaults + warning |

Unknown keys ignored; the file is never rewritten (it is yours).

### themes/&lt;name&gt;.json

Shape: `{ "name": ..., "colors": { ... } }` — hex color roles as in
`bundled/themes/dark.json`: UI roles (`user_message`, `assistant_message`,
`thinking`, `working`, `system_message`, `error`, `attention`,
`border_focused`, `border_unfocused`, `row_focused`, `status_bar_bg`,
`banner_art`, `banner_hint`) and syntax roles (`syntax_keyword`,
`syntax_string`, `syntax_comment`, `syntax_number`, `syntax_function`,
`syntax_type`, `syntax_operator`, `syntax_plain`).

---

## agents/ and prompts/

- **`agents/<name>.md`** — agent presets ("declarations"): markdown with
  frontmatter; addressed by file stem (`build`, `plan`). At least one resolvable
  preset is **required**; a broken *non-default* preset only warns. A preset may
  name its own `model: <provider>/<model>`, overriding `default_model`
  (`--model` / `/model` override both).
- **`prompts/<name>.md`** — system prompt fragments (e.g. `compression.md`,
  `subagent.md`). Missing `compression.md` → warning.

---

## Fatal vs warning at startup

**Fatal:** `settings.json` / `providers.json` / `permissions.json` /
`hooks.json` parse errors; no usable provider; no agent preset or unresolvable
`default_agent_preset`; config/data dir missing or unwritable; session DB
failure; a **bundled** plugin failing to load; service/tool/command name
collisions; duplicate provider-kind registration.

**Warning (startup continues):** malformed `plugins.json` / `mcps.json` /
`channels.json`; unregistered `backend` kind; `default_model` typo; broken
non-default presets; missing `prompts/compression.md`; MCP command/url and
channel command problems; unknown permission advisor; unknown
`compaction_strategy`; `max_concurrent_subagents` out of range; unknown
`per_tool_overrides` keys; dangling aliases; a **user** plugin failing to load.
