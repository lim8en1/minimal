# Configuration reference

Everything minimal reads at startup lives in one config directory, resolved in
this order (both server and client):

1. `$MINIMAL_CONFIG_DIR` — explicit override
2. `$XDG_CONFIG_HOME/minimal`
3. `$HOME/.config/minimal` — default (not the macOS `~/Library` path)

Runtime state (SQLite session store, logs, plugin KV storage) lives in a
separate data dir: `$XDG_DATA_HOME/minimal`, else `$HOME/.local/share/minimal`. So do
stored keys (`secrets.json`, mode 0600 — see [Secrets](#secrets)). The TUI keeps its up-arrow prompt history there too (`prompt_history.json`: the newest 500 submitted lines, at most 1 MiB); delete the file to clear it. And the last agent/model you picked, per project (`last_selection.json`): every new tab starts on it, `--agent`/`--model` outrank it, and a remembered pick that no longer resolves (a preset deleted, a provider removed) is dropped with a one-line notice instead of failing the tab. Picking **default** in the `/model` picker (or `/model default`) returns to `settings.json` `default_model` and is remembered as such. The headless client (`-p`) never reads it.

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
| `mcps.json` | server (`mcp` plugin) | optional | read by the `mcp` plugin; missing → no MCP; an invalid entry (unknown field included) → warning naming it, that server is skipped, the others load |
| `channels.json` | server (`mcp` plugin) | optional | same degrade-to-warning behavior as `mcps.json` |
| `plugins.json` | server | optional | malformed, or any entry invalid → **fatal**, no plugin loads and the file is not rewritten (see docs/PLUGINS.md) |
| `plugins.d/<name>.json` | server | optional | one per plugin: that plugin's own settings (its README documents them); read only for enabled plugins; missing → defaults; not a JSON object, an unknown field or a bad value → that plugin does not load (named field) |
| `agents/<name>.md` | server | **at least one required** | none resolvable → **fatal** |
| `prompts/<name>.md` | server | optional | missing `prompts/compression.md` → warning |
| `cost-rates.json` | server | optional | cost-tracker's fallback price table |
| `decision-advisor.json` | server (`decision-advisor` plugin) | required when the plugin is enabled | every advisor default: backend profiles, retry, thresholds, questions; missing, unknown key or bad value → the plugin does not load (named field) |
| `plugins/` (dir) | server | optional | user plugins scanned here (docs/PLUGINS.md) |
| `client-plugins.json` | client | optional | malformed or an invalid entry → warning; the file is not rewritten, and a plugin whose entry is invalid stays as the entry said (`enabled`, when that much reads) or off -- never back on by default |
| `client-plugins.d/<name>.json` | client | optional | one per client plugin, the same rules as `plugins.d/` (a bad file → that plugin does not load, with a warning) |
| `client-plugins/` (dir) | client | optional | user client plugins scanned here |
| `keybindings.json` | client | optional | missing → built-in defaults; malformed → load fails (named path) |
| `ui.json` | client | optional | malformed → warning + all defaults; a bad or unknown field → warning, that field keeps its default |
| `themes/<name>.json` | client | optional | unknown theme name → terminal defaults + warning |

Startup errors are gathered by a sanity check: **fatal** problems abort the
server; **warnings** are logged and startup continues.

**Every config file is strict.** An unknown field, a wrong-typed value, a
fractional number where an integer belongs, or an out-of-range value is an
error naming the file and the full path to the field:

```
settings.json: memory.auto_compact: must be true or false, got "yes"
providers.json: providers.local.models.qwen4.max_tokens: must be an integer, got 4096.5
plugins.d/tools-bash.json: timeout_ms: unknown field (known: background_max_timeout_s, ...)
```

Nothing falls back to a default silently. This covers the server files,
every plugin's own `plugins.d/<name>.json`, `secrets.json`, agent
preset frontmatter, and (as warnings) the client files. Keys starting with
`//` are comments, in every JSON file.

---

## settings.json

Full example (`bundled/settings.json`):

```json
{
  "default_agent_preset": "build",
  "max_concurrent_subagents": 4,
  "background_wake": false,
  "memory": {
    "auto_compact": true,
    "context_compaction_trigger": { "percent": 80 },
    "scrollback_page_size": 100
  },
  "tools": {},
  "tools_view": { "stdout_buffer_lines": 100 },
  "project_local": { "mode": "trusted" },
  "context_contributors": { "disabled": [], "max_bytes_per_section": 16384 },
  "logging": { "level": "info", "max_size_mb": 10, "max_files": 5 }
}
```

### Top level

| Field | Type | Default | Notes |
|---|---|---|---|
| `default_agent_preset` | string | `""` | file stem of an `agents/<name>.md` preset a new session starts with when none is chosen. Unset or unresolvable (a warning): the only preset there is, else the TUI asks on the first message (picker; remembered per project) and headless needs `--agent` |
| `default_model` | string | `""` | `"<provider>/<model>"` (see providers.json) — the model a session runs on when none is chosen. Unset or unresolvable (a warning): the only configured model (static or discovered; its effort variants count once), else the TUI asks on the first message (picker; remembered per project) and headless needs `--model`. A model under a `discover: true` entry warns as unverifiable |
| `small_model` | string | `""` | opencode-compat: `"<provider>/<model>"` for the compaction summarizer only; unset/unresolvable → each session compacts on its own model. The summarizer reads the whole working history, so the small model's window must fit the compaction trigger; a different model also gets no prompt-cache reuse |
| `default_subagent_model` | string | `""` | `"<provider>/<model>"` for spawned subagents whose preset declares no `model:`; unset/unresolvable → children run on the parent's model |
| `max_concurrent_subagents` | int | `4` | ≥ 0; `0` (subagents disabled) or `> 8` → warning |
| `background_wake` | bool | `false` | |
| `pause_auto_resume_on_submit` | bool | `true` | a user turn submitted while the session is paused clears the pause instead of waiting for an explicit resume (`sessions.pause_auto_resume_on_submit`) |
| `session_db_path` | string | `""` | empty → resolved under the data dir |

### `memory.*`

| Field | Type | Default | Notes |
|---|---|---|---|
| `auto_compact` | bool | `true` | |
| `context_compaction_trigger` | object | absent = **never** (auto-compaction then never fires; the bundled file sets `percent: 80`) | **exactly one** of `percent` (1–100), `tokens` (> 0), `buffer_tokens` (> 0) |
| `compaction_keep_recent_tokens` | int | `0` | absolute keep-verbatim budget (estimated tokens, newest first); when > 0 it overrides `compaction_replace_percent`. Keep it well below the trigger |
| `compaction_replace_percent` | int | `80` | 1–100. Share of the working history (by estimated tokens, oldest first) that a compaction **replaces** with the summary; the rest stays verbatim after it. The summarizer always reads the **whole** history — its request is the session's own system prompt, tools and messages plus one appended instruction, so a provider with prefix caching serves it from cache. `100` replaces everything but the newest message. On the Anthropic provider, thinking blocks in the kept part are dropped from the working set (newer Claude models reject replaying thinking after the preceding context changed); chat-completions backends never replay thinking, so nothing changes there |
| `compaction_strategy` | string | `"summarize"` (seeded: `"structured"`) | name of a registered strategy (core built-in `summarize`, or plugin-registered — `structured` is the `compaction-structured` plugin); unknown name → warning, falls back. An agent preset's `compaction_strategy:` frontmatter overrides this per agent. `structured` asks the model to call the strict `record_summary` tool, which is therefore declared on every request (names and types only, ~190 tokens; the field guidance is sent with the compaction instruction; a stray call outside compaction is a no-op); a model that answers in text instead degrades to the free-text summary. A custom `compression_prompt:` (preset frontmatter or `/compact <name>`) always runs as free text |
| `estimate_zero_usage` | bool | `true` | when a turn's backend usage is all zero (some OpenAI-compatible proxies), seed occupancy from the request's bytes/4 estimate so the compaction trigger arms, and flag the turn's wire usage `usage_estimated`. Estimates are never priced by cost tracking and never enter cumulative totals |
| `scrollback_page_size` | int | `0` | ≥ 0; `0` = the built-in page size |

Compaction instructions are prompt files in the config dir, sent verbatim as
the last message of the summarizer request: `prompts/compression.md` (free
text; also the fallback) and `prompts/compression-structured.md` (the
`structured` strategy's `record_summary` instruction and field guidance).
Both are seeded from `bundled/prompts/` and never overwritten; a missing
`compression-structured.md` makes the compaction-structured plugin log a
warning at load, and the structured strategy runs as free text.

Auto-compaction guards (manual `/compact` skips the first two):

- The trigger must be above the context no compaction can remove (system
  prompt, project context, tool definitions). At or below it, auto-compaction
  does not run and the session shows one notice.
- If the trigger fires again at the very next check after an auto compaction,
  the fold cannot get the context under the trigger: auto-compaction stops
  with a notice (instead of summarizing every tool round) and re-arms once
  the context is back under the trigger.
- A compaction that shrank the history (context minus the fixed part) by
  less than 10% carries a warning.

Notices and warnings render as compaction rows in the TUI, are kept in
scrollback, and appear on the headless `compaction` JSONL line.

### `tools.*`

Core's own limits on tool calls. Nothing else belongs here (an unknown key is
an error): tool plugins take their knobs from their own file,
`plugins.d/<name>.json`.

| Field | Type | Default | Notes |
|---|---|---|---|
| `max_parallel_tool_calls` | int | `4` | 1..64; `1` = inline execution, no pool threads |
| `max_input_bytes` | int | `10485760` | ≥ 0; `0` disables the limit |
| `max_result_bytes` | int | `10485760` | ≥ 0; `0` disables the limit |
| `max_display_bytes` | int | `131072` | ≥ 0; a tool result's client-only display payload over this is dropped (the client shows the text); `0` disables the limit |

### Per-plugin tool settings (`plugins.d/<name>.json`)

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
| `timeout_s` | int | `120` — default per-command timeout, seconds |
| `max_timeout_s` | int | `600` — ceiling on a model-supplied `timeout`; must be ≥ `timeout_s` |
| `background_max_timeout_s` | int | `600` — ceiling for background commands; must be ≥ `timeout_s` |
| `max_background_tasks` | int | `4` — per-session cap on live background tasks |
| `max_output_bytes` | int | `1048576` (1 MiB) — bounds the persisted result |
| `max_file_size_bytes` | int | `1073741824` (1 GiB) — RLIMIT_FSIZE backstop |
| `shell` | string | `"/bin/sh"` |
| `inherit_env` | string | `"allowlist"` — or `"all"` |

**`tools-python`** — flat object (every field optional; every duration key ends in `_s`, seconds):

| Key | Type | Default |
|---|---|---|
| `python` | string | `"python3"` — fallback interpreter (bare path + venv creation) |
| `sync_wait_ceiling_s` | int | `300` — ceiling on a model-supplied sync `timeout`; `0` = unlimited; expiring DEMOTES to async, never kills |
| `startup_timeout_s` | int | `10` — spawn + ready-handshake window (a broken venv is caught here) |
| `install_timeout_s` | int | `300` — venv creation / package installation |
| `grace_s` | int | `2` — how long `python_interrupt` waits for evidence the SIGINT landed |
| `background_max_runtime_s` | int | `0` (= unlimited) — default kill deadline for background requests |
| `rlimit_cpu_s` | int | `0` (= disabled) — RLIMIT_CPU backstop for the interpreter process |
| `max_output_bytes` | int | `1048576` (1 MiB) — persisted result cap per request |
| `max_capture_bytes` | int | `1048576` (1 MiB) — driver-side per-stream stdout/stderr capture cap |
| `stream_head_bytes` / `stream_tail_bytes` | int | `262144` / `786432` — head/tail windows kept of a long stream |
| `max_file_size_bytes` | int | `1073741824` (1 GiB) — RLIMIT_FSIZE backstop |

**`tools-search`** — `{"grep": {...}, "glob": {...}}`, every field a positive integer:
`grep.default_head_limit` (250), `grep.max_results` (1000), `grep.max_file_bytes`
(1 MiB), `grep.timeout_ms` (10000), `grep.max_context_lines` (10),
`grep.max_line_length` (2000), `glob.default_limit` (200), `glob.max_results`
(2000), `glob.timeout_ms` (10000).

The per-preset DECLARED package contract is NOT plugin config — it lives in
`agents/<name>.md` frontmatter under `tool_config.python`
(`packages`: list of pip specs, `requirements`: list of project-relative
requirements paths), delivered to the tools per invocation. See
[agents/](#agents-and-prompts) and the `tools-python` description below.

### `tools_view.*`

| Field | Type | Default |
|---|---|---|
| `stdout_buffer_lines` | int | `100` |

### `project_local.*`

Whether a repository's own `agents/`, `skills/` and `commands/` directories
(the project-local discovery below) are loaded. They are content the repository's
author chose, so by default they are **ignored until you trust the directory**.

| Field | Type | Default | Notes |
|---|---|---|---|
| `mode` | string | `"trusted"` | `"trusted"`: load only for directories you have trusted with `/trust-directory`. `"all"`: load for every project (the behaviour before this setting existed; use it for CI or a machine where every checkout is yours). `"none"`: never load project-local content |

The unit of trust is the project's **git worktree root**, so one grant covers a
repository and every directory a session starts in beneath it; a directory with
no git root is its own root. `/trust-directory` shows the directory and what it
would enable and asks for confirmation; `/untrust-directory` revokes. Grants live
in `<config dir>/trusted_projects.json` (operator state — not hand-edited
settings); an unreadable file trusts nothing. A grant takes effect for what is
resolved afterwards (`/agent`, a new session, the next listing) — the running
session's current preset is not swapped. A new session in an untrusted project
that ships such content gets a one-line notice naming `/trust-directory`.
The project-context plugin (AGENTS.md / CLAUDE.md) is not governed by this
section. Presets in the config dir and bundled presets are always
trusted; a *trusted* project's presets are still only allowed to narrow, never
widen, your permissions.

### `context_contributors.*`

Plugin-contributed sections of the system context (the project-context
plugin's AGENTS.md, the `memory` plugin's index, ...), rendered once per
session and ordered by each contributor's declared priority: `0` first,
core's signal-channel instructions at `50`, plugins by default at `100`; ties
keep plugin load order. A contributor that declares its own size cap
(project-context's `max_bytes`) is exempt from `max_bytes_per_section`.

| Field | Type | Default | Notes |
|---|---|---|---|
| `disabled` | string array | `[]` | plugin names whose sections are skipped; non-empty strings |
| `max_bytes_per_section` | int | `16384` | per-section body cap, cut at a UTF-8 boundary with a marker; `0` = unlimited; negative → **fatal** |

### `logging.*`

| Field | Type | Default | Allowed values |
|---|---|---|---|
| `level` | string | `"info"` | `trace` \| `debug` \| `info` \| `warn` \| `error` \| `critical` \| `off` |
| `path` | string | `""` | empty → `minimal.log` in the data dir; relative → under the data dir |
| `max_size_mb` | int | `10` | ≥ 1 |
| `max_files` | int | `5` | ≥ 1 |

### `sessions.*` and `server.*`

All values must be ≥ 0 (negative → **fatal**); `0` means "never" where noted.

| Field | Type | Default | Notes |
|---|---|---|---|
| `sessions.detached_idle_ttl_seconds` | int | `1800` | idle reaper collects a fully quiescent detached session after this; `0` = never |
| `sessions.unattended_prompt_timeout_seconds` | int | `0` | bound on permission/confirmation/plugin-prompt waits, counted only while NO client is attached to the session (an attached client is waited for indefinitely); `0` = wait forever |
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
then no model is usable). The schema is strict: an unknown field at any level
(top, provider, model, `timeouts`, variant body) is a named startup error. `bundled/providers.example.json`
is a commented starting point.

### Per provider

| Field | Required | Type | Default | Notes |
|---|---|---|---|---|
| `backend` | **yes** | string | — | a provider *kind*: `"openai"`, `"anthropic"` (bundled) or anything a plugin registered. Unknown kind → warning |
| `url` | **yes** | string | — | `openai` backend: includes `/v1`. `anthropic` backend: **without** `/v1` — the provider appends `/v1/messages` itself |
| `api_key` | no | string | — | a key reference — see [Secrets](#secrets): `"secret:NAME"`, `"secret:<plugin>:NAME"`, `"env:NAME"`, or a literal key (keep the file private if you use one). A reference that does not resolve yet is a startup warning and that provider's requests fail naming it; other providers are unaffected. Absent: no auth header, which a local server needs |
| `discover` | no | bool | `false` | `true`: models are listed lazily from the backend (`GET <url>/models`) instead of a `models` object; never written back; failures fall back to any static entries; the cache is warmed in the background at server start (`server.discovery_warmup_timeout_seconds`), and the `/model` picker's refresh (ctrl+r) re-fetches |
| `model_info` | no | bool | `false` | `openai` backend, with `discover`: also fetch `GET <url>/model/info` (a LiteLLM proxy's endpoint) for each listed model's reasoning-effort flags, so its `@effort` variants are autodetected. A failed or absent endpoint leaves discovery as `/models` alone |
| `models` | yes (unless `discover`) | object | — | keyed by model name |
| `parameters` | no | object | `{}` | arbitrary sampling/generation knobs passed through to the backend (e.g. `temperature`, `top_p`, `cache_ttl` for anthropic: `"5m"` or `"1h"`). Model-level `parameters` **replaces** (does not merge) the provider's. Two consumed keys steer routing/caching instead of passing through: `session_id` (below) and, for the `openai` backend, `cache_control: true` (opt-in Anthropic-style content-part caching) with `cache_ttl: "5m"`/`"1h"` |
| `session_id` (inside `parameters`) | no | bool/string | `true` | default **on**: each request (turns, subagents, compaction) carries the harness session id for provider sticky routing — OpenRouter routes the session to the endpoint whose prompt cache is warm and groups the requests in its Logs "Sessions" view. `openai` backend: top-level body field; `anthropic` backend: `x-session-id` header (the real API rejects unknown body fields but ignores custom headers). `false` opts out — use it for OpenAI-compatible backends that reject unknown body fields. A **string** value passes through the body verbatim as a static routing key |
| `headers` | no | object or list | `{}` | extra HTTP headers sent on every request to this provider — chat *and* `GET <url>/models` discovery (e.g. `{"X-Title": "my-app"}`, or `[["X-Title", "my-app"]]`). Values must be scalars (numbers/bools are sent as text); a name duplicating one the backend already sets (Content-Type, Authorization) is yours to avoid |
| `openrouter_app_id` | no | bool | `false` | `true`: every request carries OpenRouter's app-attribution headers (`X-Title: minimal`, `HTTP-Referer: https://github.com/lim8en1/minimal`) so usage is credited to the project on OpenRouter's app leaderboard. A same-named `headers` entry wins per name; any other type is a parse error |
| `max_tokens` | no | int | — | required for the `anthropic` backend (here or per-model) or every request fails |
| `timeouts` | no | object | see right | transport timeouts in milliseconds, each a positive integer: `connect_ms` (5000), `request_ms` (120000, non-streaming calls), `stream_idle_ms` (60000, silence between bytes once the response started), `first_byte_ms` (600000, wait for the first response byte after the request was sent — raise for local backends prefilling very large prompts) |
| `max_tokens_field` | no | string | `"max_tokens"` | the JSON body key the output cap is emitted under (`openai` backend only). Newer OpenAI models reject the legacy `max_tokens` key and require `"max_completion_tokens"` — set that here. Any other value is a parse error. Ignored by the `anthropic` backend |
| `context_window_tokens` | no | int | — | per provider = default for all its models |
| `variants` | no | array or object | — | effort variants applied to every model under the entry that declares none of its own — see [Reasoning-effort variants](#reasoning-effort-variants) below. For a `discover: true` entry this is the autodiscovery recipe (with none, discovery autodetects: every model whose catalog advertises an effort ladder gets that ladder verbatim) |
| `whitelist` / `blacklist` | no | string list | — | picker filters on base model ids (see Filtering below) |
| `metadata` | no | bool | `true` | enrich models missing a window/effort ladder from the model catalog |
| `metadata_match` | no | string | `"full"` | `"full"` or `"substring"` id matching against the catalog |
| `metadata_provider` | no | string | — | pin the catalog's provider table |

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
same list-or-map convention the skills plugin's slice uses):

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
  per-name scale (low 4096, medium 8192, high 16384, xhigh 32768, max 65536;
  clamped to leave 1k of `max_tokens` for output — the API rejects
  `budget_tokens >= max_tokens`; `none` omits thinking; `minimal` is refused);
  everything else gets `reasoning_effort: "<name>"` (`xhigh`/`max` send
  `high`, the top of OpenAI's ladder; `none` sends `minimal`, the lowest rung
  every backend accepts).
  An unknown name is a parse error naming the valid ones.
- **Map form** — opencode-style per-variant bodies: `parameters` (passed
  through verbatim, model-parameter rules apply), plus optional
  variant-scoped `max_tokens` and `context_window_tokens`. Any other key in a
  variant body is a parse error.
- **Discovery** — a provider-level `variants` block is the recipe for every
  model a `discover: true` entry lists whose catalog metadata advertises an
  effort ladder (OpenRouter's `reasoning.supported_efforts`). With no recipe,
  discovery autodetects: each such model gets its OWN advertised ladder
  verbatim (`z-ai/glm-5.3-flash` → `@max`, `@high`, `@low` with no config); a
  model advertising no ladder gets none. An entry-level or model-level recipe
  overrides the ladder, and a static model on a discover entry that declares
  no variants of its own picks the ladder up too (its parameters ride along).
- **Filtering** — `whitelist`/`blacklist` match the BASE model id: hiding a
  model hides its variants.
- **`/think`** — switches the current session: bare `/think` lists the active
  model's variants (`*` marks the current one), `/think <variant>` switches
  (persisted like `/model`, so it survives `/agent` and resume), `/think off`
  returns to the base model.

---

## Secrets

Every config field that carries a key — providers.json `api_key`, and the
plugin settings that send one (`memory`'s `api_key`, `tools-web`'s
`search.api_key`, `decision-advisor`'s `api_key`) — takes the same reference:

| Reference | Resolves to | When it is read |
|---|---|---|
| `"secret:NAME"` | `NAME` in the data dir's `secrets.json` | at every use: `/secret set` reaches the next request, no restart |
| `"secret:<plugin>:NAME"` | `NAME` from that plugin's secret source (e.g. `secret:opencode-compat:anthropic` = opencode's `auth.json`) | at every use |
| `"env:NAME"` | the environment variable | once, at startup; unset → startup warning, each use fails |
| `"word:..."` (any other letters-only scheme) | refused at startup — a typo'd scheme like `secrets:` is never sent as the key; store a literal of that shape with `/secret set` | — |
| anything else | the literal key | — |

A stored secret is reached only by naming it: nothing is picked up
implicitly, so an entry with no `api_key` sends no key. A `secret:` reference
that is not stored yet, or an `env:` variable that is unset, is a startup
**warning**, and each use fails naming the fix.

`secrets.json` is written by `/secret` (or by hand; it is re-read on use).
Every entry must be an object with a string `value` and an optional `hosts`
list; anything else (a bare string, `hosts` given as a string) is an error,
never skipped — a skipped `hosts` would let a host-bound key go anywhere:

```json
{ "anthropic":  { "value": "sk-...", "hosts": ["api.anthropic.com"] },
  "embeddings": { "value": "..." } }
```

`hosts` (optional) binds where the secret may be sent: each entry is a host or
`host:port`, matched against the destination URL (no port = any port). A
destination that matches none is refused, so an entry redirected elsewhere
cannot carry the key there. Without `hosts`, the explicit reference is the
consent.

```
/secret set <name> <value> [--host <host[:port]>]...   replace the entry
/secret list                                         names and hosts, never values
/secret delete <name>
```

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
  "plugins": {
    "mcp": { "default": "ask" },
    "tools-web": { "hosts": { "*.internal": "deny" } }
  }
}
```

Mode vocabulary:

- **Permission mode** (what a rule assigns): `"allow"` | `"ask"` | `"deny"`
- **Run mode** (how the session treats the substrate): `"normal"` | `"bypass"` | `"allow-only"`

| Field | Type | Default | Notes |
|---|---|---|---|
| `default_run_mode` | string | `"normal"` | one of the run modes |
| `fallback` | object | harness defaults | five categories, each a permission mode: `read_only`, `file_write`, `execute`, `out_of_workdir`, `network`. Unset categories keep the harness default (bundled: `read_only=allow`, rest `ask`) |
| `per_tool_overrides` | object | `{}` | tool permission key (bare tool name or `plugin.tool`) → permission mode (`allow`/`ask`/`deny`, lower-case). Keys that resolve to no registered tool → warning. Aliased tools keep their **original** name here (see docs/PLUGINS.md). MCP tools are keyed by their full name (`mcp__<server>__<tool>`) and refine the server's mode. Precedence: a preset's per-tool rule wins; a preset's category (or per-server MCP mode) and a global per-tool rule that both apply resolve to the **more restrictive** of the two; then the global category |
| `pattern_overrides` | object | `{}` | tool permission key → **glob → mode** rules matched against the tool's subject (bash: the command line **and its stages** — see below; `read`/`write`/`edit`/`grep`/`glob`: the canonical path). Semantics: when any rule matches, the **most restrictive matching rule** replaces the category default, folded most-restrictively with an exact `per_tool_overrides` entry if there is one (so `"bash": "allow"` cannot defeat a matching deny); zero matches leave the exact entry or category default. Downstream folds (out-of-workdir, network, subagent ceilings, run modes) apply as always — a pattern can never bypass the substrate. An `ask` rule is never satisfied by a session grant: a held grant on `/usr/bin/git` does not carry `git push --force` past `"git push *": ask`. What most-restrictive folding cannot express: a strict broad rule plus a more-specific allow (`"git *": ask` + `"git status": allow` stays ask) — set `pattern_precedence: "specific"` for that |
| `pattern_precedence` | string | `"restrictive"` | how matching pattern rules for one tool combine. `"restrictive"`: the most restrictive match wins. `"specific"`: a matching **deny** still wins outright; otherwise the most specific match wins (most literal characters; ties go to the stricter rule), so `"git *": ask` + `"git status": allow` allows `git status` and asks for everything else. For bash, every stage needs an allow winner and any ask or deny winner decides; normalized respellings still only tighten. Also settable per preset (`permission_mode.pattern_precedence`); a project-local preset cannot choose `"specific"` |
| `external_directory_patterns` | object | `{}` | glob → mode rules on the `out_of_workdir` dimension, matched against the canonical **absolute** path of an invocation that resolved outside the project root; the most restrictive matching rule replaces the scalar `out_of_workdir` for that invocation |
| `plugins` | object | `{}` | plugin name → that plugin's permission-model slice, verbatim (core checks only that it is an object). `mcp`: `{"default": mode, "transport": {"local": mode, "remote": mode}, "signals": {"<channel>/<signal>"|"<channel>"|"*": mode}}` — `default` is the mode for MCP servers a preset does not name, `transport` overrides it by how the server is reached (`local` = stdio, `remote` = http), and `signals` gates `channels.json` notifications per receiving session (most specific key wins; no rule = delivered; a preset's `plugin_permissions.mcp.signals` refines it and a project preset may only narrow). `tools-web`: `{"default": mode, "hosts": {glob: mode}}` — per-host network rules for `web_fetch`/`web_search` (most specific glob wins; unmatched → `default`, else `fallback.network`) |
| `advisor` | string | — | plugin name of a permission advisor; unknown name → warning |
| `advisor_inherit` | bool | `false` | also let the advisor answer inside subagent sessions. An advisor decides one invocation at a time and never mints session grants |
| `advisor_timeout_ms` | int | `30000` | ≥ 0; `0` disables the advisor budget |

**Seeing what applies.** Every denial and every permission prompt names the
rule that decided it (`preset: file_write=deny`, ``pattern `git push *`=ask``,
`outside the project: out_of_workdir=ask`, `parent session: …`, `run mode
allow-only …`). `/permissions` shows the session's effective rules and held
grants; `/permissions check <tool> <json-input>` dry-runs a call through the
real gate (e.g. `/permissions check bash {"command":"git push"}`);
`/permissions revoke <key>|all` drops grants.

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

Inert output (`2>/dev/null`, `2>&1`, `2>&-`) does not affect matching. A real
output redirect (`>`, `>>`, `&>`, `2>` to a path) or an **incomplete** parse
(`$X args`, `eval "$CMD"`) makes allow rules inert for the whole command —
`"git log *": allow` does not cover `git log > f` — while ask/deny rules still
match the line and every stage. `time`/`coproc` in the command slot are demoted
(their real program is the stage); a quoted spelling (`'time' make`) is not a
keyword and stays in the stage. **ask/deny rules also see normalized
spellings** of every stage — words unquoted, the program by its basename, and
wrapper programs (`sudo`, `env`, `command`, `builtin`, `exec`, `nice`, `nohup`,
`time`, `timeout`, `stdbuf`, `ionice`, `xargs`) peeled off with their flags —
so `"rm *": deny` also catches `/bin/rm x`, `'rm' x`, `\rm x`, `sudo rm x` and
`ls | xargs rm -f`. Allow rules never see these spellings (they could only
widen through them). Still a guardrail against mistakes, not a boundary:
`sh -c 'rm x'`, `$(echo rm) x` and `find -exec rm` stay out of reach of any
string rule.

**Built-in defaults**: `read`, `write`, `edit` and `grep` deny `**/.env` and
`**/.env.*`. They merge per glob with operator rules: a rule on the same glob
replaces the default (`"**/.env": "allow"` lifts it), and rules on other globs
leave it in force.

**Walks honor path rules per file.** A path rule is matched against the path a
call names, and for `grep`/`glob` that is the directory the walk starts from.
So the rules for those tools are also applied to **every file the walk reaches**.
A file is withheld when its rules are stricter than the call's own: a `deny`
match, or an `ask` match (a walk cannot prompt for each file). The output says
how many were withheld. To reach one, name the file itself as the call's
`path`; that call prompts, or is denied, like any other. The dotenv defaults
work the same way, so `grep` over `.` never reads `.env`. A tool with no
pattern rules pays nothing for this.

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
| `default_timeout_ms` | no | int, default `10000`; must be ≥ 0 (`0` = no budget; a cancellable emission can still be abandoned, an uncancellable one gets a 60 s floor) |
| `hooks` | no | array of actions |

Each action:

| Field | Required | Notes |
|---|---|---|
| `on` | **yes** | hook point name. Unknown/never-registered point → warning, not fatal |
| `command` | **yes** | run via `/bin/sh -c` with the JSON payload on stdin; never argv-interpolated |
| `timeout_ms` | no | int ≥ 0; default = `default_timeout_ms` |
| `filter` | no | object of string→string, exact-match, ANDed; only on fields the point declares filterable |

Any other field in an action is an error — including Claude Code's `matcher`,
which here is spelled `filter: {"tool_name": "..."}`.

Core hook points (plugins may register more, e.g. `tools-bash.before_command`):

| Point | Filterable fields |
|---|---|
| `core.pre_tool_use` (veto-capable) | `tool_name`, `permission_key`, `category` (`read_only` \| `file_write` \| `execute` \| `mcp`) |
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

Owned by the `mcp` plugin, which validates it and reports problems as startup
warnings under its own name: stdio `command` not on PATH, http `url` not
well-formed. With the plugin disabled the file is ignored.

## channels.json

Top-level **object keyed by channel name** (external event ingress; stdio-only
in v1). Name must match `[a-z0-9._-]`; `"sessions"` is reserved; a name that
also appears in `mcps.json` → warning, channel skipped. Owned by the `mcp`
plugin, like `mcps.json`; its channels are delivered through core's signal
router.

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
register runtime ids (a view's id, or its `client-plugins.json` alias when one
renames it). Bundled actions (see `bundled/keybindings.json`):
`tab.next`, `tab.previous`, `tab.goto.1` … `tab.goto.9` (`alt+1` … `alt+9`, the
number the tab bar shows), `group.previous`, `group.next`, `jump.attention`,
`session.picker`, `agent.picker`, `model.picker`,
`permission.cycle`, `panel.focus.next`, `panel.focus.prev`,
`tab.close`, `chat.load_older`, `turn.cancel`, `session.pause`, `app.quit`.
The prompt editor and the chat view declare their own actions, bound the same
way -- the bundled editor: `prompt.newline`, `prompt.undo`, `prompt.redo`,
`prompt.clear`; the bundled chat view: `chat.scroll_up`, `chat.scroll_down`,
`chat.scroll_bottom`, `chat.row_up`, `chat.row_down`, `chat.toggle_collapse`,
`chat.toggle_collapse_all`, `chat.toggle_thinking_fold`,
`chat.toggle_tool_fold`, `chat.toggle_timestamps`.

Modifiers: `ctrl` (or `control`), `alt` (or `option`/`opt`/`meta` — the
macOS key's spelling), `shift`, joined by `+` in any order. macOS: Apple
builds match `alt+letter`/`alt+digit` bindings against both the ESC-prefixed
Alt encoding *and* the glyphs an unconfigured Option key sends (US layout:
option+g sends `©`, option+j `∆`, …), so bindings fire without the
terminal's "Use Option as Meta Key" setting (Terminal.app, iTerm2 — default
"Normal" left/right Option key —, Ghostty, Alacritty, and WezTerm all send
these glyphs out of the box). The glyph is then claimed by the binding and
no longer typeable; on non-US layouts whose plain keys send those characters
(ß on German), rebind rather than fight it.

macOS keyboard-shortcut collisions: the defaults deliberately avoid two keys
macOS claims before the terminal ever sees them. `⌃↑`/`⌃↓` are bound
systemwide to Mission Control / Application Windows (System Settings →
Keyboard → Keyboard Shortcuts → Mission Control), so `chat.scroll_up`/
`chat.scroll_down` default to `ctrl+alt+k`/`ctrl+alt+j` instead of
`ctrl+up`/`ctrl+down`. `F1` is a hardware brightness key on Mac keyboards
unless Fn is held or "Use F1, F2, etc. keys as standard function keys" is
enabled, so the key-bindings overlay (the bundled `help-view` plugin's
`help.overlay` view) defaults to `alt+h` instead of `f1`. Both old
specs still parse and can be rebound in `keybindings.json` — e.g. after
disabling the relevant System Settings shortcut — they're just no longer
the shipped defaults.
The `/keymap` command opens the same overlay as the `help.overlay` binding:
a filterable listing of every configured action → key pair, each described
in core's words (a plugin view by its title).

Prompt newline (`prompt.newline`, default `alt+enter`): Enter submits;
Alt+Enter inserts a newline (the prompt soft-wraps to the terminal width, at
most half the screen). Besides ESC CR / ESC LF, the default matches the
CSI-u encodings terminals emit when the kitty keyboard protocol or xterm
modifyOtherKeys is on (`13;3u`, `13;2u`, `27;3;13~`, `27;2;13~`) —
Shift+Enter works on those terminals with no configuration. macOS caveat:
Terminal.app, iTerm2, Ghostty, Alacritty and WezTerm send Option+Enter as a
plain `\r` until Option is configured as Alt (Terminal.app: "Use Option as
Meta Key"; iTerm2: Profiles → Keys → Left/Right Option Key → `Esc+`;
Ghostty: `macos-option-as-alt`; Alacritty: `option_as_alt`; WezTerm:
`LeftOption` as `LeftAlt`) — with no byte distinguishing that from plain
Enter, no in-app match can tell them apart. If enabling that terminal
setting isn't an option, rebind `prompt.newline` instead — e.g. to
`"shift+enter"` (works out of the box on a kitty-protocol-aware terminal) or
to any letter/digit spec unrelated to Enter (e.g. `"ctrl+alt+n"`), which
sidesteps the ambiguity rather than needing a distinguishing byte at all.

Prompt undo (`prompt.undo`, default `alt+z`; `prompt.redo`, default
`ctrl+alt+z`): reverts the last edit to the prompt, per tab. Typing coalesces
into one step per word (a typed space, a caret jump or a pause over a second
starts a new one); a whole paste, a kill line (`ctrl+u`/`ctrl+k`), an
accepted completion and a history recall are one step each. Up to 100 steps;
submitting clears them. Not `ctrl+z` — that is the terminal's suspend key.

Prompt clear (`prompt.clear`, default `ctrl+alt+u`): empties the whole
prompt wherever the caret is (`ctrl+u`/`ctrl+k` kill only one side of it).
One undo step — `prompt.undo` brings the text back. Prompt editor only.

### ui.json

| Field | Type | Default | Notes |
|---|---|---|---|
| `status_format` | string | built-in default | segment ids in braces with literal text between; the default is `{activity} · {tokens} · {cost?} · {token_detail?}{>}{model} · {agent} · {permission_mode}`. Core ids: `activity`, `tokens`, `model`, `agent`, `permission_mode`, `session`, `paused`, `version`; plugin ids (e.g. cost-view's `cost`, `token_detail`) too. An unknown id is warned about at startup unless it is written `{id?}` -- optional, for a plugin segment that may not be installed. A plugin segment cannot take a core id: it is ignored with a warning. `{>}` right-aligns everything after it. An empty segment takes its separator with it; when the bar overflows, whole segments drop lowest-priority first (`version`, `token_detail`, `session` … `permission_mode` last) — a value is never clipped |
| `scrollback_window_max_messages` | int | `0` (= built-in) | must be positive if set |
| `tool_output_buffer_bytes` | int | `0` | |
| `panel_height` | int | unset (12) | rows for a panel's content (the tools panel and every plugin panel); minimum 3 |
| `theme` | string | `""` (= `"dark"`) | names `themes/<name>.json`; unknown name → terminal defaults + warning |

Unknown keys ignored; the file is never rewritten (it is yours) — the only
exception is the managed write of `theme` (`/theme`), which round-trips every
other key verbatim and refuses to touch a malformed file. (The fold defaults
are the chat-view plugin's config now: `fold_thinking` / `fold_tools` in its
`client-plugins.json` entry; the `ctrl+alt+t` / `ctrl+alt+o` toggles are kept
in the plugin's own state.)

### themes/&lt;name&gt;.json

Shape: `{ "name": ..., "colors": { ... } }` — hex color roles as in
`bundled/themes/dark.json`: UI roles (`user_message`, `assistant_message`,
`thinking`, `working`, `system_message`, `muted`, `error`, `attention`,
`border_focused`, `border_unfocused`, `row_focused`, `status_bar_bg`,
`background`, `banner_art`, `banner_hint`, `tool`, `success`, `cursor`, `heading`), syntax roles (`syntax_keyword`,
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
- `heading` colors markdown headings (falls back to `border_focused`).
- `background` is **optional**: when set it paints every cell the UI leaves
  unpainted; when omitted the terminal's own (possibly transparent)
  background shows through.

A missing role falls back to the terminal default. With `NO_COLOR` set or
`TERM=dumb`, no colors are emitted at all. Otherwise the depth follows
`COLORTERM` (`truecolor`/`24bit`), then the terminal's own identity (`TERM`
naming kitty, alacritty, wezterm, foot, ghostty or a `-direct` entry;
`TERM_PROGRAM` iTerm.app, WezTerm, vscode, ghostty, Hyper or Tabby), then a
`256color` `TERM`, else 16 colors.

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
  name its own `model: <provider>/<model>` — **it applies only when the preset
  is spawned as a subagent** (a cheap explorer, a fast reviewer). A primary
  session runs on the model you chose (`--model`, `/model`, or the last pick
  remembered for the project) or else the default model (`default_model`, or
  the only one configured), and
  switching agents (`/agent`, the agent picker) never switches models. An
  unresolvable preset `model:` (a project preset written on another machine)
  warns at startup and its spawns fall back to `default_subagent_model`, then
  the parent's model. A preset may also name its own `compaction_strategy: <name>`
  — the strategy kind its compaction summaries use (`summarize`, or a
  plugin's: `structured`; see `memory.compaction_strategy`). Absent = the
  settings-level strategy stands; a name nothing registered warns once per
  session and degrades the same way.
- **`plugins`** — `[<plugin name>, ...]`: every tool those plugins register
  (by their `plugins.json` name: `tools-fs`, `memory`, ...), added to
  `allowed_tools` when the preset resolves — so a plugin that later gains a
  tool reaches the preset without an edit. Only visibility: `permission_mode`
  still decides each call. MCP tools never join a group (they follow
  `tool_sources`). An entry naming no loaded plugin, or one that registers no
  tools, warns at startup and allows nothing. A subagent's groups are clamped
  by its parent like any `allowed_tools` entry. Prefer explicit `allowed_tools`
  names where a preset must NOT get a plugin's future tools (the bundled
  read-only presets list `memory_search`/`memory_get` rather than `memory`).
- **`allowed_tools`** — the allow-list of core/plugin tools the preset runs
  with. **Fail-closed: omitted or empty means NO core tools.** `permission_mode`
  only decides what happens when an admitted tool is called (allow/ask/deny);
  it never admits one — a preset with `permission_mode` but no `allowed_tools`
  serves zero core tools. `["*"]` means "all registered tools" and must be
  written explicitly. Entries reference permission keys, never surface aliases
  (see `Aliases` under the `plugins.json` docs in `docs/PLUGINS.md`); an
  unknown entry warns at startup ("matches no registered tool" — a typo, or
  the providing plugin is disabled). MCP tools are never governed by
  `allowed_tools` — not even by `["*"]` — they follow `tool_sources` /
  `plugin_permissions`. Two names are conditional: `skill` (skills plugin)
  only surfaces when the preset may load a skill, and `spawn_subagent` is stripped from
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
  same fail-closed rule as the skills plugin withholding `skill`. An unlisted
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

- **More preset fields** (opencode's names, native here) — also accepted in the frontmatter:
  `mode: all|primary|subagent` (subagent = spawnable but hidden from the agent
  picker; never gates spawnability), `disable: true` (refuses to resolve, not
  listed), `hidden: true` (not listed, still spawnable), `color: "#rrggbb"`
  (agent picker), `steps: <int>` (ceiling on tool rounds when the preset runs
  AS A SUBAGENT; a spawn request may lower it, never raise it), and a
  `permission:` block: `edit`/`write`→file_write, `external_directory`→
  out_of_workdir, and a key naming a loaded tool — its permission key, or a
  spelling the tool declares (tools-web: `webfetch`, `websearch`) — governs
  that tool only (a scalar is a per-tool rule, a `{glob: mode}` map its
  patterns, unless the tool takes no patterns). A key naming neither warns and
  is ignored; a native `permission_mode` field wins over the block. Note that
  `permission_mode` (native or aliased) only sets ask/allow/deny for tools the
  preset already admits — see `allowed_tools` above.
- **`parameters:`** — provider request parameters for this preset, a map
  passed through verbatim and overriding the model config's values
  (`parameters: {reasoning_effort: high, seed: 7}`). opencode's top-level
  `temperature`, `top_p` and `reasoningEffort` are accepted too (`parameters:`
  wins on a clash). Request fields the harness owns (`tools`, `tool_choice`,
  `messages`, `stream`, `model`) are refused. Values keep their YAML type, and a
  quoted value stays a string (`"007"` is not 7).
- **Every other frontmatter key is an error.** A typo'd key (`allowed-tools`)
  used to ride to the provider as a request field; `tools:` (opencode / Claude
  Code) is refused with a pointer to `allowed_tools` and `permission_mode`.
  List fields (`allowed_tools`, `plugins`, ...) must be lists: `allowed_tools: "*"`
  is an error, not "no tools".
- **`tool_sources:`** — dynamic tool members a plugin offers, per source:
  `tool_sources: {mcp: [github, search]}` allows those `mcps.json` servers'
  tools. Each member is brought up at session start and on `/agent`; one that
  cannot come up is reported as a session error and its tools stay hidden.
  A subagent keeps only members its parent also has. A source no loaded
  plugin provides, or a member it does not know, warns at startup.
- **`plugin_permissions:`** — each plugin permission model's preset slice,
  verbatim: `plugin_permissions: {mcp: {servers: {github: allow}}, tools-web:
  {hosts: {"docs.python.org": allow}}}`. A preset's rule wins over
  `permissions.json`'s `plugins` slice; a project-local preset may only
  narrow.
- **`tool_config:`** — per-tool/per-plugin configuration, a map of
  `{<namespace>: {...}}` addressed by the namespace a plugin's tools declare
  (`Tool::config_namespace()`; currently `python` for the tools-python
  plugin). Structural: never passed through to the provider. A namespace no
  registered tool claims warns at startup (typo, or the providing plugin is
  disabled); two plugins claiming one namespace is fatal. The plugin owns its
  slice's schema — see the tools-python section below for the `python` slice.
- **Project-local presets** (loaded only for trusted directories — see
  `project_local.*`) — a session's project root (walked up to the
  enclosing git worktree root, `ProjectDirs`) is searched for
  `<dir>/agents/<name>.md` BEFORE the config dir; the nearest declaration wins
  and a project preset shadowing a global one is logged. Project presets are
  merged into the agent picker listing (the picker sends the client's project
  root) and resolve with the session's root for `/agent` and subagent spawns.
  Project-local problems surface at resolve time as resolve errors/warnings,
  not at startup.
- **Project-local skills** (same trust rule) — the same walk scans `<dir>/skills/*/SKILL.md` plus
  the Claude-compatible `<dir>/.claude/skills/*/SKILL.md` and
  `<dir>/.agents/skills/*/SKILL.md`; the nearest directory wins on a name
  collision and a project skill shadows a global one. The `skill` tool's
  preset-scoped description (see the preset-scoped tool definitions note under
  `allowed_subagents`), the skills slice's resolution, and the tool itself all
  consult the same session-scoped view, so the description never advertises a
  skill the tool would refuse.
- **Skill access (the `skills` plugin)** — a preset's
  `plugin_permissions: {skills: ...}` slice over `permissions.json`'s
  `plugins.skills` (a preset entry replaces the operator's for that name). The
  list form means every entry is `allow`; the map form gives per-skill `allow`
  (listed, tool loads outright), `ask` (listed, marked "(asks before
  loading)"; loading prompts with the stable grant key `skill:<name>`, so
  "always" covers later loads of that skill only), `deny` (hidden; the tool is
  refused), and `preload` (the full body ships in the system prompt and
  survives compaction like any constant section; not tool-callable). `"*"` is
  legal in both forms and an explicit entry beats it for that name. Subagent
  clamping is elementwise most-restrictive over `preload < allow < ask <
  deny` — a child can never widen into its parent's preloads, and a preload
  under an allow-only parent demotes to plain allow.
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
catalog, then cached for that root and rebuilt when a file there changes —
`ProjectCommandSource`); the nearest
directory wins a stem collision, and a project command may shadow a global
markdown command of the same name (never a built-in or plugin command). The composed prompt is submitted as the turn itself via
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
  overrides: the turn the command submits runs on the borrowed model and/or
  preset (a borrowed preset brings its prompt and policy, not its `model:`),
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
`hooks.json` parse errors; a malformed `plugins.json` or an invalid entry in it; no usable provider; no agent preset or unresolvable
`default_agent_preset`; config/data dir missing or unwritable; session DB
failure; a **bundled** plugin failing to load; service/tool/command name
collisions; duplicate provider-kind registration; two plugins claiming one
`tool_config` namespace; two plugins registering one tool source; a plugin
permission model claiming another plugin's tool.

**Warning (startup continues):** anything a loaded
plugin reports about its own config (the `mcp` plugin: malformed `mcps.json` /
`channels.json`, MCP command/url and channel command problems) — printed under
the plugin's name; unregistered `backend` kind; `default_model` typo; broken
non-default presets; missing `prompts/compression.md`; a preset's
`tool_sources` naming an unprovided source or unknown member; unknown permission advisor; unknown
`compaction_strategy` (settings or an agent preset's frontmatter naming no
registered kind); `max_concurrent_subagents` out of range; unknown
`per_tool_overrides` keys; unclaimed `tool_config` namespaces; dangling
aliases; a **user** plugin failing to load.
