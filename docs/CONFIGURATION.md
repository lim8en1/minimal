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
runtime. Copyable example agents, prompts, skills, and commands live in
[examples/](../examples/).

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
| `small_model` | string | `""` | opencode-compat: `"<provider>/<model>"` for the compaction summarizer only; unset/unresolvable → each session compacts on its own model |
| `default_theme` | string | `""` | client theme name (`themes/<name>.json`) |
| `max_concurrent_subagents` | int | `4` | `<= 0` or `> 8` → warning |
| `background_wake` | bool | `false` | |
| `pause_auto_resume_on_submit` | bool | `true` | a user turn submitted while the session is paused clears the pause instead of waiting for an explicit resume (`sessions.pause_auto_resume_on_submit`) |
| `session_db_path` | string | `""` | empty → resolved under the data dir |

### `memory.*`

| Field | Type | Default | Notes |
|---|---|---|---|
| `auto_compact` | bool | `true` | |
| `context_compaction_trigger` | object | absent = never | **exactly one** of `percent` (1–100), `tokens` (> 0), `buffer_tokens` (> 0) |
| `context_compaction_trigger_tokens` | int | `0` | legacy flat key, honored only when the object form is absent; `0` = never |
| `compaction_keep_recent_tokens` | int | `0` | |
| `compaction_strategy` | string | `"summarize"` | name of a registered strategy (core built-in or plugin-registered); unknown name → warning, falls back |
| `estimate_zero_usage` | bool | `true` | when a turn's backend usage is all zero (some OpenAI-compatible proxies), seed occupancy from the request's bytes/4 estimate so the compaction trigger arms, and flag the turn's wire usage `usage_estimated`. Estimates are never priced by cost tracking and never enter cumulative totals |
| `scrollback_page_size` | int | `0` | |

### `tools.*`

`tools` is also passed **verbatim to plugins** as `tools_settings` — unknown
subkeys are legal and plugin-owned. Core recognizes:

| Field | Type | Default | Notes |
|---|---|---|---|
| `mcp.connect_timeout_ms` | int | `10000` | |
| `mcp.request_timeout_ms` | int | `60000` | |
| `mcp.max_list_pages` | int | `100` | must be ≥ 1 |
| `mcp.restart_cooldown_ms` | int | `30000` | must be ≥ 0 |
| `max_parallel_tool_calls` | int | `4` | must be ≥ 1; `1` = inline execution, no pool threads |
| `max_input_bytes` | int | `10485760` | ≥ 0; `0` disables the limit |
| `max_result_bytes` | int | `10485760` | ≥ 0; `0` disables the limit |
| `max_display_bytes` | int | `131072` | ≥ 0; a tool result's client-only display payload over this is dropped (the client shows the text); `0` disables the limit |

**No bundled tool plugin reads a `tools.read` / `tools.edit` / `tools.bash`
slice from settings.json.** The `tools-fs` and `tools-bash` plugins take their
knobs from their own `config` object in `plugins.json` (the per-plugin channel
of EXTENSIBILITY §2.3). Core still parses `tools.read`/`edit`/`bash` into its
settings struct, but nothing consumes them — put tool settings in
`plugins.json`.

### Per-plugin tool settings (`plugins.json` `config`)

**`tools-fs`** — object with one sub-object per tool
(`{"read": {...}, "edit": {...}}`; every field optional):

| Key | Type | Default |
|---|---|---|
| `read.default_limit_lines` | int | `2000` |
| `read.max_line_length` | int | `2000` |
| `read.max_lines` | int | `2000` — the hard ceiling on any read window; `limit: 0` clamps to this too |
| `edit.max_editable_file_bytes` | int | `5242880` (5 MiB) |

**`tools-bash`** — flat object (every field optional):

| Key | Type | Default |
|---|---|---|
| `timeout_ms` | int | `120000` |
| `max_timeout_ms` | int | `600000` — ceiling on a model-supplied `timeout_ms` |
| `background_max_timeout_ms` | int | `600000` |
| `max_background_tasks` | int | `4` — per-session cap on live background tasks |
| `max_output_bytes` | int | `1048576` (1 MiB) — bounds the persisted result |
| `max_file_size_bytes` | int | `1073741824` (1 GiB) — RLIMIT_FSIZE backstop |
| `shell` | string | `"/bin/sh"` |
| `inherit_env` | string | `"allowlist"` — or `"all"`; anything else is a named parse error |

**`tools-python`** — flat object (every field optional; ALL times in SECONDS):

| Key | Type | Default |
|---|---|---|
| `python` | string | `"python3"` — fallback interpreter (bare path + venv creation) |
| `sync_wait_ceiling` | int | `300` — ceiling on a model-supplied sync `timeout`; `0` = unlimited; expiring DEMOTES to async, never kills |
| `startup_timeout` | int | `10` — spawn + ready-handshake window (a broken venv is caught here) |
| `install_timeout` | int | `300` — venv creation / package installation |
| `grace_seconds` | int | `2` — how long `python_interrupt` waits for evidence the SIGINT landed |
| `max_output_bytes` | int | `1048576` (1 MiB) — persisted result cap per request |
| `max_capture_bytes` | int | `1048576` (1 MiB) — driver-side per-stream stdout/stderr capture cap |
| `rlimit_cpu_seconds` | int | `0` (= disabled) — RLIMIT_CPU backstop for the interpreter process |
| `max_file_size_bytes` | int | `1073741824` (1 GiB) — RLIMIT_FSIZE backstop |

The per-preset DECLARED package contract is NOT plugin config — it lives in
`agents/<name>.md` frontmatter under `tool_config.python`
(`packages`: list of pip specs, `requirements`: list of project-relative
requirements paths), delivered to the tools per invocation. See
[agents/](#agents-and-prompts) and the `tools-python` description below.

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
| `server.idle_shutdown_seconds` | int | `5` | exit after this long with zero attached clients and no live session still doing work (a detached-but-busy session holds the server until it finishes); `0` = never (daemon) |
| `server.discovery_warmup_timeout_seconds` | int | `15` | wall-clock budget for the background warm-up of `discover: true` providers started right after the server begins listening, so the `/model` picker lists discovered models without a manual ctrl+r; startup never waits on the fetch, and a fetch exceeding the budget keeps running in the background; `0` = disabled (static models only until ctrl+r or first use) |

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
| `discover` | no | bool | `false` | `true`: models are listed lazily from the backend (`GET <url>/models`) instead of a `models` object; never written back; failures fall back to any static entries; the cache is warmed in the background at server start (`server.discovery_warmup_timeout_seconds`), and the `/model` picker's refresh (ctrl+r) re-fetches |
| `models` | yes (unless `discover`) | object | — | keyed by model name |
| `parameters` | no | object | `{}` | arbitrary sampling/generation knobs passed through to the backend (e.g. `temperature`, `top_p`, `cache_ttl` for anthropic: `"5m"` or `"1h"`). Model-level `parameters` **replaces** (does not merge) the provider's. Two consumed keys steer routing/caching instead of passing through: `session_id` (below) and, for the `openai` backend, `cache_control: true` (opt-in Anthropic-style content-part caching) with `cache_ttl: "5m"`/`"1h"` |
| `session_id` (inside `parameters`) | no | bool/string | `true` | default **on**: each request (turns, subagents, compaction) carries the harness session id for provider sticky routing — OpenRouter routes the session to the endpoint whose prompt cache is warm and groups the requests in its Logs "Sessions" view. `openai` backend: top-level body field; `anthropic` backend: `x-session-id` header (the real API rejects unknown body fields but ignores custom headers). `false` opts out — use it for OpenAI-compatible backends that reject unknown body fields. A **string** value passes through the body verbatim as a static routing key |
| `headers` | no | object | `{}` | extra HTTP headers sent on every request to this provider — chat *and* `GET <url>/models` discovery (e.g. `{"X-Title": "my-app"}`). Values must be scalars; a name duplicating one the backend already sets (Content-Type, Authorization) is yours to avoid |
| `openrouter_app_id` | no | bool | `false` | `true`: every request carries OpenRouter's app-attribution headers (`X-Title: minimal`, `HTTP-Referer: https://github.com/lim8en1/minimal`) so usage is credited to the project on OpenRouter's app leaderboard. A same-named `headers` entry wins per name; any other type is a parse error |
| `max_tokens` | no | int | — | required for the `anthropic` backend (here or per-model) or every request fails |
| `max_tokens_field` | no | string | `"max_tokens"` | the JSON body key the output cap is emitted under (`openai` backend only). Newer OpenAI models reject the legacy `max_tokens` key and require `"max_completion_tokens"` — set that here. Any other value is a parse error. Ignored by the `anthropic` backend |
| `context_window_tokens` | no | int | — | per provider = default for all its models |
| `variants` | no | array or object | — | effort variants applied to EVERY model under the entry — see [Reasoning-effort variants](#reasoning-effort-variants) below. For a `discover: true` entry this is the autodiscovery recipe (with none, discovery adds `@high` to every model whose catalog advertises an effort ladder) |

### Per model (`models.<name>`)

| Field | Required | Notes |
|---|---|---|
| `context_window_tokens` | no | the one field whose staleness disables compaction — set it. Model value wins over the provider default |
| `max_tokens` | no | inherits from provider when absent |
| `parameters` | no | **replaces** the provider's `parameters` when present |
| `variants` | no | effort variants of THIS model (replaces the provider-level recipe) — see below |

The model *key* is sent on the wire verbatim as the request's `model` field —
it must be the backend's real model id, unlike the provider name which is
yours to choose.

### Reasoning-effort variants

One model, several thinking efforts. A `variants` block — on a model, or on the
provider for all its models — synthesizes sibling models addressed as
`<provider>/<model>@<name>`: same connection, **same wire model id** (the
`@suffix` is addressing only, never sent), different reasoning parameters. They
appear in `/model`, `--model`, and `/think` like any model.

Two spellings, one key (JSON's array/object distinction separates them, the
same list-or-map convention `allowed_skills` uses):

```json
"claude-opus-4-5": {
  "context_window_tokens": 200000,
  "max_tokens": 32000,
  "variants": ["low", "medium", "xhigh"]
}
```

```json
"z-ai/glm-5.3-flash": {
  "context_window_tokens": 1310720,
  "variants": {
    "deep": { "parameters": { "reasoning": { "effort": "high", "max_tokens": 32768 } } },
    "lean": { "parameters": { "reasoning_effort": "low" }, "max_tokens": 16384 }
  }
}
```

- **List form** — known effort names: `low`, `medium`, `high`, `xhigh`, `max`,
  `minimal`, `none`. Wire parameters are derived per backend: `anthropic`
  entries get `thinking: {type: "enabled", budget_tokens: N}` from a fixed
  per-name budget scale (clamped to fit `max_tokens` — the API rejects
  `budget_tokens >= max_tokens`); everything else gets
  `reasoning_effort: "<name>"` (`xhigh`/`max` normalize to `high`, the top of
  OpenAI's ladder; `none` means reasoning off where the backend has a switch).
  An unknown name is a parse error naming the valid ones.
- **Map form** — opencode-style per-variant bodies: `parameters` (passed
  through verbatim, model-parameter rules apply), plus optional
  variant-scoped `max_tokens` and `context_window_tokens`. Any other key in a
  variant body is a parse error.
- **Discovery** — a provider-level `variants` block is the recipe for every
  model a `discover: true` entry lists whose catalog metadata advertises an
  effort ladder (OpenRouter's `reasoning.supported_efforts`); with no recipe,
  discovery adds `@high` alone to each such model. The recipe intersection is
  computed at publish: a model advertising no ladder gets no variants.
- **Filtering** — `whitelist`/`blacklist` match the BASE model id: hiding a
  model hides its variants.
- **`/think`** — switches the current session: bare `/think` lists the active
  model's variants (`*` marks the current one), `/think <variant>` switches
  (persisted like `/model`, so it survives `/agent` and resume), `/think off`
  returns to the base model.

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
| `pattern_overrides` | object | `{}` | tool permission key → **glob → mode** rules matched against the tool's subject (bash: the command line **and its stages** — see below; `read`/`write`/`edit`/`grep`/`glob`: the canonical path). Semantics: an exact `per_tool_overrides` entry still wins; otherwise the **most restrictive matching rule replaces the category default** when any rule matches, and zero matches leave the category default. Downstream folds (out-of-workdir, network, subagent ceilings, run modes) apply as always — a pattern can never bypass the substrate. What most-restrictive folding cannot express: a strict default rule plus a more-specific allow (`"git *": ask` + `"git status": allow` stays ask) — use an exact override instead |
| `external_directory_patterns` | object | `{}` | glob → mode rules on the `out_of_workdir` dimension, matched against the canonical **absolute** path of an invocation that resolved outside the project root; the most restrictive matching rule replaces the scalar `out_of_workdir` for that invocation |
| `mcp_default_mode` | string | `"ask"` | default permission mode for MCP tools |
| `advisor` | string | — | plugin name of a permission advisor; unknown name → warning |
| `advisor_inherit` | bool | `false` | |
| `advisor_timeout_ms` | int | `30000` | ≥ 0; `0` disables the advisor budget |

Glob rules (`pattern_overrides` / `external_directory_patterns`): `*` and `?`
stay within one path component, `**` crosses them, `\` escapes, and a pattern
matches the whole subject. For bash command lines `/` is an ordinary character
(so `git *` covers `git status src/x.cpp`); for paths it is a component
boundary.

**Bash stages.** The scanner (tree-sitter-bash) decomposes the command line
into one subject per `command` node — pipeline stages, `;`/`&&`/`||` list
members, subshell and `$(...)` bodies, `env-prefix` command after the
assignments. Rules apply per mode:

- **allow**: a conjunction — *every* stage must match some allow rule, or no
  rule applies at all. `"git status" + "wc *"` covers `git status | wc -l`;
  `git status | tee out` prompts (the `tee` stage is uncovered). An allow rule
  can no longer match through a whole-line prefix: `git diff; make install`
  never rides `git diff *`.
- **ask / deny**: match the whole line **or any stage** — `"rm *": deny`
  denies `ls | rm -rf /` even though `ls` matches nothing.

Inert output (`2>/dev/null`, `2>&1`, `2>&-`) does not affect stages; a real
output redirect (`>`, `>>`, `&>`, `2>` to a path) does not block matching, but
an **incomplete** parse (`$X args`, `eval "$CMD"`) yields no stages, so allow
rules cannot fire and only ask/deny whole-line rules apply. `time`/`coproc` in
the command slot are demoted (their real program is the stage); a quoted
spelling (`'time' make`) is not a keyword and stays in the stage. **Built-in defaults**: `read`, `write` and `edit` deny `**/.env` and
`**/.env.*` — living in the default layer, any operator `pattern_overrides`
entry for that tool replaces them wholesale (write your own deny back in if
you also allow something).

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

**Environment:** hook commands run with the server process's full environment
(no allowlist — unlike the `tools-bash` tool, which allowlists by default).
They also run as the server user with the server's working directory, so a
hook can read anything the server can; treat hook commands with the same care
as the server binary itself.

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
`tab.next`, `tab.previous`, `tab.goto.1` … `tab.goto.9` (`alt+1` … `alt+9`, the
number the tab bar shows), `group.previous`, `group.next`, `jump.attention`,
`session.picker`, `agent.picker`, `model.picker`, `help.overlay`,
`permission.cycle`, `panel.toggle`, `panel.focus.next`, `panel.focus.prev`,
`tab.close`, `chat.load_older`,
`chat.scroll_up`, `chat.scroll_down`, `chat.scroll_bottom`, `chat.row_up`,
`chat.row_down`, `chat.toggle_collapse`, `chat.toggle_collapse_all`,
`chat.toggle_thinking_fold`, `chat.toggle_tool_fold`,
`chat.toggle_timestamps`, `turn.cancel`, `app.quit`.

Modifiers: `ctrl` (or `control`), `alt` (or `option`/`opt`/`meta` — the
macOS key's spelling), `shift`, joined by `+` in any order. macOS: Apple
builds match `alt+letter`/`alt+digit` bindings against both the ESC-prefixed
Alt encoding *and* the glyphs an unconfigured Option key sends (US layout:
option+g sends `©`, option+j `∆`, …), so bindings fire without the
terminal's "Use Option as Meta Key" setting. The glyph is then claimed by
the binding and no longer typeable; on non-US layouts whose plain keys send
those characters (ß on German), rebind rather than fight it.
The `/keymap` command opens the same overlay as the `help.overlay` binding:
a filterable listing of every configured action → key pair.

Prompt newline: Enter submits; Alt+Enter inserts a newline (the prompt
soft-wraps to the terminal width, at most half the screen). Besides ESC CR /
ESC LF, the editor recognizes the CSI-u encodings terminals emit when the
kitty keyboard protocol or xterm modifyOtherKeys is on (`13;3u`,
`13;2u`, `27;3;13~`, `27;2;13~`) — Shift+Enter works on those terminals
with no configuration. macOS caveat:
Terminal.app, Ghostty, Alacritty and WezTerm send Option+Enter as a plain
`\r` until Option is configured as Alt (Terminal.app: "Use Option as Meta
Key"; Ghostty: `macos-option-as-alt`; Alacritty: `option_as_alt`; WezTerm:
`LeftOption` as `LeftAlt`) — with no byte distinguishing it from Enter, no
in-app binding can catch it.

### ui.json

| Field | Type | Default | Notes |
|---|---|---|---|
| `status_format` | string | built-in default | segment ids in braces with literal text between; the default is `{activity} · {tokens} · {cost} · {token_detail}{>}{model} · {agent} · {permission_mode}`. Core ids: `activity`, `tokens`, `model`, `agent`, `permission_mode`, `session`, `version`; plugin ids (e.g. cost-view's `cost`, `token_detail`) too. `{>}` right-aligns everything after it. An empty segment takes its separator with it; when the bar overflows, whole segments drop lowest-priority first (`version`, `token_detail`, `session` … `permission_mode` last) — a value is never clipped |
| `scrollback_window_max_messages` | int | `0` (= built-in) | must be positive if set |
| `tool_output_buffer_bytes` | int | `0` | |
| `tools_panel_stdout_buffer_lines` | int | `0` | distinct from the server's `tools_view.stdout_buffer_lines` |
| `fold_thinking` | string | `"auto"` | the startup auto-fold default for thinking rows: `auto` (built-in default), `expanded` or `collapsed`; anything else → warning + `auto`. `chat.toggle_thinking_fold` (`ctrl+alt+t`) flips it for every open tab and persists the new value back here |
| `fold_tools` | string | `"auto"` | same for tool-activity rows; `chat.toggle_tool_fold` (`ctrl+alt+o`). `collapsed` also folds `ask_user` results — the explicit setting outranks the built-in "always show answers" default |
| `theme` | string | `""` (= `"dark"`) | names `themes/<name>.json`; unknown name → terminal defaults + warning |

Unknown keys ignored; the file is never rewritten (it is yours) — the only
exceptions are the managed writes: `theme` (`/theme`), and `fold_thinking` /
`fold_tools` (their `ctrl+alt+` toggles), each of which round-trips every
other key verbatim and refuses to touch a malformed file.

### themes/&lt;name&gt;.json

Shape: `{ "name": ..., "colors": { ... } }` — hex color roles as in
`bundled/themes/dark.json`: UI roles (`user_message`, `assistant_message`,
`thinking`, `working`, `system_message`, `muted`, `error`, `attention`,
`border_focused`, `border_unfocused`, `row_focused`, `status_bar_bg`,
`background`, `banner_art`, `banner_hint`, `tool`, `success`, `cursor`), syntax roles (`syntax_keyword`,
`syntax_string`, `syntax_comment`, `syntax_number`, `syntax_function`,
`syntax_type`, `syntax_operator`, `syntax_plain`) and diff roles
(`diff_added`, `diff_removed`, `diff_hunk`).

- `assistant_message` is also the base color of message bodies and the prompt
  editor's text; `syntax_plain` is the base color of code blocks.
- `muted` is secondary text: hints, footers, line-number gutters, collapse
  markers (falls back to `system_message`). The UI never uses SGR dim, whose
  look is up to the terminal.
- `border_unfocused` colors separators and rules.
- `cursor` on `status_bar_bg` is the picker / option cursor and the chat row
  cursor (falls back to `row_focused`); with neither set, or `status_bar_bg`
  unset, the terminal's reverse video is used instead.
- `tool` colors tool-call headers (falls back to `syntax_function`); keep it
  distinct from `user_message` so tool rows never read as your own turns.
- `success` colors a successful tool outcome (falls back to `working`).
- `background` is **optional**: when set it paints every cell the UI leaves
  unpainted; when omitted the terminal's own (possibly transparent)
  background shows through.

A missing role falls back to the terminal default. With `NO_COLOR` set or
`TERM=dumb`, no colors are emitted at all.

Bundled themes: `dark`, `light`, `lucent-crm`, `lucent-crm-light`,
`lucent-grn`, `lucent-grn-light` — each sets its own `background` (dark
for the dark ones, near-white for `-light`), so they read the same on any
terminal. `slate` and `moss` are mid-tone themes for **transparent**
terminal backgrounds: they set no `background`, and every text role stays
legible on both black and white, so they survive whatever the desktop
shows through.

---

## agents/ and prompts/

- **`agents/<name>.md`** — agent presets ("declarations"): markdown with
  frontmatter; addressed by file stem (`build`, `plan`). At least one resolvable
  preset is **required**; a broken *non-default* preset only warns. A preset may
  name its own `model: <provider>/<model>`, overriding `default_model`
  (`--model` / `/model` override both).
- **`allowed_tools`** — the allow-list of core/plugin tools the preset runs
  with. **Fail-closed: omitted or empty means NO core tools.** `permission_mode`
  only decides what happens when an admitted tool is called (allow/ask/deny);
  it never admits one — a preset with `permission_mode` but no `allowed_tools`
  serves zero core tools. `["*"]` means "all registered tools" and must be
  written explicitly. Entries reference permission keys, never surface aliases
  (see `Aliases` under the `plugins.json` docs in `docs/PLUGINS.md`); an
  unknown entry warns at startup ("matches no registered tool" — a typo, or
  the providing plugin is disabled). MCP tools are never governed by
  `allowed_tools` — not even by `["*"]` — they follow `allowed_mcp_servers` /
  `mcp_permissions`. Two names are conditional: `skill` only surfaces when
  `allowed_skills` is non-empty, and `spawn_subagent` is stripped from
  subagent sessions (no recursion is a hard rule, not preset config). The
  valid set is whatever is registered under `plugins.json`'s enablement; with
  every bundled plugin enabled:

  | Source | Names |
  |---|---|
  | core (always registered) | `spawn_subagent`, `skill` |
  | tools-bash | `bash` |
  | tools-fs | `read`, `write`, `edit` |
  | tools-search | `grep`, `glob` |
  | tools-web | `web_fetch`, `web_search` |
  | tools-python | `python_exec`, `python_interrupt`, `python_output`, `python_input` |
  | checklist | `checklist` |
  | signals-message | `message_session` |
  | tool-ask | `ask_user` |

- **`allowed_subagents`** — the allow-list of agent preset KEYS (the
  `agents/<name>.md` file stems, what `spawn_subagent`'s `agent_preset`
  accepts — NOT the frontmatter display `name:`, which need not match) this
  preset may spawn. Absent = every spawnable preset; `["*"]` is explicit
  "all"; a present list is the exact set, and a present EMPTY list means no
  spawns at all — `spawn_subagent` is withheld from the session entirely, the
  same fail-closed rule as `allowed_skills` resolving empty. An unlisted
  preset is refused by `SubagentManager::Spawn` with a named error before any
  child session exists. An entry naming no preset warns at resolve time (a
  typo would silently shrink the set). The spawn tool's per-preset definition
  (below) reflects the same list: it names this preset as the default,
  lists exactly the allowed presets, and carries a JSON-schema `enum` of them
  — with the default line omitted when the preset does not list itself, since
  enforcement would refuse the very default being advertised.

- **Preset-scoped tool definitions** — any core tool may ship a per-preset
  definition: a builder registered on `AgentPresetRegistry`
  (`SetDefinitionBuilder`, wired in `main.cpp` beside the tool's own
  registration) runs ONCE per resolved preset — lazily, inside the resolve
  cache keyed (preset, project root) — and its result rides
  `AgentPreset::tool_definition_overrides`; `AgentLoop::BuildSystemAndTools`
  swaps it in for the tool's static definition by name. The definition is
  constant per session, so the request prefix the provider's prompt cache
  keys on never shifts. `skill` and `spawn_subagent` use it today — the
  skill listing that used to render into the system prompt now rides the
  skill tool's description, and the spawn tool's description/enum are built
  the same way. The builders' shared per-root inputs (the spawnable-presets
  scan — which, unlike the picker listing, includes `hidden` and
  `mode: subagent` presets, and the merged skill view) are memoized per
  canonical project root.

- **opencode-compat preset fields** — also accepted in the frontmatter:
  `mode: all|primary|subagent` (subagent = spawnable but hidden from the agent
  picker; never gates spawnability), `disable: true` (refuses to resolve, not
  listed), `hidden: true` (not listed, still spawnable), `color: "#rrggbb"`
  (agent picker), `steps: <int>` (ceiling on tool rounds when the preset runs
  AS A SUBAGENT; a spawn request may lower it, never raise it), a deprecated
  opencode `tools:` map (`name: true|false` → per-tool allow/deny overrides),
  and a `permission:` block (`read/glob/grep/list`→read_only, `edit`/`write`→
  file_write, `bash`→execute, `webfetch`/`websearch`→network,
   `external_directory`→out_of_workdir; a native `permission_mode` field wins
   over the alias). Note that `permission_mode` (native or aliased) only sets
   ask/allow/deny for tools the preset already admits — see `allowed_tools`
   above. Any OTHER frontmatter key is passed through to the provider
   as a request parameter (`temperature`, `top_p`, `reasoningEffort`, ...),
   overriding the model config's value.
- **`tool_config:`** — per-tool/per-plugin configuration, a map of
  `{<namespace>: {...}}` addressed by the namespace a plugin's tools declare
  (`Tool::config_namespace()`; currently `python` for the tools-python
  plugin). Structural: never passed through to the provider. A namespace no
  registered tool claims warns at startup (typo, or the providing plugin is
  disabled); two plugins claiming one namespace is fatal. The plugin owns its
  slice's schema — see the tools-python section below for the `python` slice.
- **Project-local presets** — a session's project root (walked up to the
  enclosing git worktree root, `ProjectDirs`) is searched for
  `<dir>/agents/<name>.md` BEFORE the config dir; the nearest declaration wins
  and a project preset shadowing a global one is logged. Project presets are
  merged into the agent picker listing (the picker sends the client's project
  root) and resolve with the session's root for `/agent` and subagent spawns.
  Project-local problems surface at resolve time as resolve errors/warnings,
  not at startup.
- **Project-local skills** — the same walk scans `<dir>/skills/*/SKILL.md` plus
  the Claude-compatible `<dir>/.claude/skills/*/SKILL.md` and
  `<dir>/.agents/skills/*/SKILL.md`; the nearest directory wins on a name
  collision and a project skill shadows a global one. The `skill` tool's
  preset-scoped description (see the preset-scoped tool definitions note under
  `allowed_subagents`), the preset's `allowed_skills` resolution, and the tool
  itself all consult the same session-scoped view, so the description never
  advertises a skill the tool would refuse.
- **`allowed_skills` modes** — the list form means every entry is `allow`; the
  map form gives per-skill `allow` (listed, tool loads outright), `ask` (listed,
  marked "(asks before loading)"; the tool narrows its own permission resolution
  to Ask with the stable grant key `skill:<name>`, so "always" covers later
  loads of that skill only), `deny` (hidden; the tool refuses), and `preload`
  (the full body ships in the system prompt at session start and survives
  compaction like any constant section; not tool-callable). `"*"` is legal in
  both forms and an explicit entry beats it for that name. Subagent clamping is
  elementwise most-restrictive over `preload < allow < ask < deny` — a child
  can never widen into its parent's preloads, and a preload under an
  allow-only parent demotes to plain allow.
- **`prompts/<name>.md`** — system prompt fragments (e.g. `compression.md`,
  `subagent.md`). Missing `compression.md` → warning.

---

## commands/ (custom slash-commands, `commands-md` plugin)

Every `commands/*.md` file becomes a `/`-command (the file stem is the name).
Step-by-step walkthrough with runnable examples:
**[docs/COMMANDS_HOWTO.md](COMMANDS_HOWTO.md)**.
Discovery merges `<config>/commands/` (global, registered once at server
startup) with the SESSION's own project root's `commands/` and
`.opencode/commands/` (opencode's layout, so existing command files work
zero-copy; registered lazily, the first time a session rooted there needs its
catalog, then cached for that root — `ProjectCommandSource`); the nearest
directory wins a stem collision, and a project command may shadow a global
one of the same name. The composed prompt is submitted as the turn itself via
the sanctioned shorthand path — results of the RUN (errors, the submit
notice) never enter the chat.

```markdown
---
description: Deploy the thing          # autocomplete text
args: [env]                            # required, bound to positionals in order
optional: [namespace]
sources:                               # execution lives HERE, not in the body
  branch:
    run: ["/usr/bin/git", "branch", "--show-current"]   # argv; {name} per entry
  summary:
    sh: "make summary"                 # shell string; XOR with run
    output: {type: json, fields: [plan]}               # addressable fields
    timeout: 30                        # seconds; default 600; 0 = infinite
    output_cap: {max: 50000, on_exceed: confirm}       # token estimate; confirm|fail
---
Deploy {env} from {branch} per {output:summary.plan}.
```

- **The body is inert.** It is a single-pass render of already-evaluated
  values: `{name}` (args, optionals, source names, `{args}`, positionals
  `{1}`..), `{output:source.field}` for JSON sources. Inserted values are never
  re-scanned. Substitution is KNOWN-NAMES-ONLY and lenient: an unknown
  `{token}` passes through as literal text (identifier-shaped ones warn once
  per render — that is how a typo surfaces); a truly empty composed prompt,
  a missing required argument, or a failed source is a hard error and
  nothing is submitted.
- **Sources are flat**, evaluated in declaration order, no cross-references.
  Substitution in argv/scripts is lenient (only known names substitute; JSON
  and shell braces pass through). An unfilled optional renders empty in the
  body and OMITS its argv entry; a script referencing one errors.
- **No body** → the prompt is the sole text source's stdout (a CLI util that
  computes the whole prompt).
- **opencode syntax desugars at load**: `$ARGUMENTS`→`{args}`, `$1`..`$9`→
  positionals, each `` !`cmd` ``→an anonymous `sh` source. Mixing desugared
  constructs with declared sources works and is lint-warned. Frontmatter
  `model: <provider>/<model>` and `agent: <preset>` ride as PER-TURN
  overrides: the turn the command submits runs on the borrowed model/preset,
  the session's sticky choices untouched; a malformed `model:` refuses to
  load, an unresolvable one fails the turn with a named error. `subtask:`
  still warns as not-yet-supported.
- **Failures** (nonzero exit, timeout, missing JSON field, declined cap) surface
  as error status lines; nothing is submitted and nothing reaches the model.

---

## tools-python (persistent Python interpreter)

A bundled plugin giving the model a persistent per-session Python interpreter
(`python_exec`, `python_interrupt`) and `/python [restart]` for the operator.

**The environment is per-project; the state is per-session.** Every session,
tab, and subagent working in one `project_root` shares ONE venv at
`<project_root>/.venv` (created on demand with `uv venv`, else
`python3 -m venv`); each session gets its OWN interpreter process, so globals
and live connections never collide. The interpreter binary is
`<venv>/bin/python` when the venv exists, else the bare `python` fallback from
plugin config.

**Declaring the environment** — in an agent preset:

```markdown
---
name: rev
tool_config:
  python:
    packages: [impacket, pefile==2023.2.7]
    requirements: [requirements.txt]
---
```

The declared set is ensured additively at every interpreter start
(`uv pip install --python ...`, else the venv's own pip; `requirements:` paths
are passed with `-r`, resolved against the project root). The declared contract
is diffed against the running interpreter on EVERY call:

- **new package names / requirement paths** → auto-installed into the shared
  venv (immediately importable by the running interpreter) and noted in the
  result;
- **a same-name different-spec declaration** (`pefile==2024.1.1` while the
  session runs `2023.2.7`) → a one-time rejection naming the conflict — the
  shared environment is never silently re-versioned; install manually or
  restart with `clean: true`;
- a venv appearing under a bare (system-python) interpreter → a one-time
  notice; `clean: true` moves into it.

**Timeouts demote, never destroy.** `python_exec(code, timeout=0)` runs
async — the result is injected at a turn boundary whenever it finishes; there
is no deadline. `timeout > 0` waits synchronously; at the deadline (or on
turn cancel) the request CONVERTS to async — the interpreter and its state are
untouched. The interpreter itself is killed only explicitly
(`python_interrupt(kill=true)`, `clean: true`, `/python restart`) or
structurally (session terminate, server exit). A soft interrupt
(`python_interrupt`, SIGINT) fails the running request with
`KeyboardInterrupt` and preserves everything; the tool reports whether the
interrupt demonstrably landed.

---

## Fatal vs warning at startup

**Fatal:** `settings.json` / `providers.json` / `permissions.json` /
`hooks.json` parse errors; no usable provider; no agent preset or unresolvable
`default_agent_preset`; config/data dir missing or unwritable; session DB
failure; a **bundled** plugin failing to load; service/tool/command name
collisions; duplicate provider-kind registration; two plugins claiming one
`tool_config` namespace.

**Warning (startup continues):** malformed `plugins.json` / `mcps.json` /
`channels.json`; unregistered `backend` kind; `default_model` typo; broken
non-default presets; missing `prompts/compression.md`; MCP command/url and
channel command problems; unknown permission advisor; unknown
`compaction_strategy`; `max_concurrent_subagents` out of range; unknown
`per_tool_overrides` keys; unclaimed `tool_config` namespaces; dangling
aliases; a **user** plugin failing to load.
