# minimal — opencode compatibility plan

Companion to `ARCHITECTURE.md` (design source of truth) and `DEVELOPMENT_PLAN.md` (sequencing source of truth). This document records the opencode-compatibility effort: what it adopts, what it deliberately does not, and every decision made along the way with its rationale. Verified against the tree and opencode's published docs (permissions, commands, agents, skills, providers, plugins) as of Sep 2026.

**Scope ruling.** Compatibility is delivered as **native alias parsing inside minimal's existing config files** — no import tool, no parallel config format — plus **project-local discovery** for agents, skills and commands. "Compatibility" targets config conventions and behaviors, not plugins: opencode's plugin ecosystem is TypeScript via npm/Bun, minimal's is a native C++ ABI; there is no bridge, and none is planned.

## 1. Where minimal stands (review summary)

At parity or ahead: tool set (grep RE2 + gitignore, bash with background tasks / tree-sitter command parsing / env allowlist, web_fetch with SSRF policy), subagents with parent-clamped permissions, agent presets via md frontmatter, MCP stdio + Streamable-HTTP (tools), skills, hooks (11 core veto-capable points — beyond opencode's plugin events), permission substrate (categories, grants, advisor, subagent ceilings), occupancy-triggered compaction with pluggable strategies, session rewind/redo, headless mode with signals.

Gaps, ranked: custom `/commands` from md files · permission pattern rules · file-edit undo/snapshots · MCP resources+prompts · provider breadth & OAuth · share/export · LSP + formatters · TUI diff rendering · todo TUI · ACP/IDE/SDK surfaces.

## 2. Decision log

| Decision | Choice | Rationale |
|---|---|---|
| Config compat delivery | native aliases in existing files | no format fork; minimal stays authoritative; user chose over one-shot import |
| Project-local discovery | yes (repo walk-up) | required for real opencode parity; matches `.opencode/`/`.claude/` conventions |
| `preload` skill mode | added beyond allow/ask/deny | always-on skills live in the system prompt, survive compaction by construction (constant section, same as body/`project_context`) |
| Command md model | **body is inert; execution lives in frontmatter sources** | executable body content (`` !`{output:x}` {y}``) opens unbounded complexity: recursive expansion, evaluation-order ambiguity, quoting. Inert body + flat sources makes the question unaskable |
| Expansion engine | ONE pass; opencode syntax is **parse-time desugaring**, not a second runtime | `$ARGUMENTS`→`{args}`, `$n`→positional, `` !`cmd` ``→anonymous `sh` source; inserted values are never re-scanned |
| Mixed styles in one file | allowed, lint-warned | desugaring makes mixes well-defined (each construct = one flat source); user may migrate incrementally |
| Template interpolation location | plugin-local in `commands-md` | consumer-first rule (track-M precedent); promote to a shared header only when a second real consumer appears |
| Generator invocation | argv array (`run`), shell string (`sh`) XOR per source | argv kills quoting/injection and makes optional-args trivial; `sh` is needed anyway (desugaring target, pipes) and is a field-level choice, not a second syntax layer |
| `output_cap.on_exceed` default | `confirm` (estimate + once/session/cancel via ConfirmationBroker) | catches runaway generators without killing legitimate big-prompt workflows; truncation removed entirely (wasted money for garbage output); `fail` available per source |
| Command `agent:`/`model:` frontmatter | subagent presets → `spawn_subagent` (works for primary presets too — visibility-only gating); primary-without-subtask and bare `model:` warn-unsupported v1 | minimal has no per-turn model/agent; per-turn override on `SubmitTurn` is the honest follow-up (deferred §6) |
| `blacklist`/`whitelist` filters | applied core-side on static ∪ discovered union | both provider plugins unchanged; filter is a config-layer concern |
| Credentials | `auth-file` plugin reads opencode-shaped `auth.json`; `/connect` writes `<data dir>/auth.json` | AuthProvider seam exists for exactly this; core never writes credentials; seeded-config-never-overwritten holds |
| Skill clamp lattice | `deny > ask > allow > preload` | preload is *wider* than allow (always-on), so a child must not widen into it |

## 3. The plan

### Phase 1 — Project-local discovery (server core) — **DONE**

Walk up from cwd to git root (reuse the `project_context.cpp` ancestor walk):
- Agents: `<root>/agents/*.md`
- Skills: `<root>/skills/*/SKILL.md` plus Claude-compatible `<root>/.claude/skills/*/SKILL.md` and `<root>/.agents/skills/*/SKILL.md`
- Commands (Phase 4): `<root>/commands/*.md` and `<root>/.opencode/commands/*.md`

Merge over the global config dir; project shadows global on name collision — `sanity_check` reports it as a warning, not an error. Files: `agent_preset_registry.cpp`, `skill_registry.cpp`, `sanity_check`, config paths. Tests: discovery, precedence, shadowing.

**Status: complete.** As built: `config::ProjectDirs` (`src/server/config/project_dirs.{hpp,cpp}`) owns the nearest-first walk bounded at the git worktree root (`.git` as file or dir; no root found → the project root only, never `$HOME`). Because `project_root` is a **SESSION property** (client-supplied per `CreateSession`), discovery resolves per resolve-time, not at server startup: `AgentPresetRegistry::Resolve(name, project_root)` (cache keyed by name + canonical root; project shadowing logged), `ListAvailable(project_root)` for the picker (`ListAgentPresetsRequest.project_root` is a new additive proto field; the TUI sends the client's cwd — the same root it sends at session creation), `SkillRegistry::AllFor/FindFor/LoadBodyFor` (per-root merged view cached behind a mutex; heap-held so `Load()` keeps returning by value), `SessionControl::project_root()` for the `/agent` command. The startup sanity check stays global-only; project-local problems surface at resolve time. Suite green (1598 passed, 1 skipped live-backend). **Trap recorded for Phases 4+:** `for (auto x : Temp(...).dirs())` binds the loop range to a reference INTO a temporary `ProjectDirs`, whose lifetime a returned-reference binding does not extend — the loop iterates a destroyed vector. Name the walk local.

### Phase 2 — Agent frontmatter aliases (server core) — **DONE**

Accept-and-map in `agent_preset_frontmatter.cpp`; unknown fields warn-once.

| opencode field | Mapping |
|---|---|
| `permission` | → `permission_mode`: `edit`/`write`→`file_write`, `bash`→`execute`, `webfetch`/`websearch`→`network`, `external_directory`→`out_of_workdir`, `read`/`glob`/`grep`/`list`→`read_only`; pattern-object values collapse to the `"*"` entry or the MOST RESTRICTIVE rule (patterns deferred, §6); native `permission_mode` always wins over the alias |
| `mode` | `primary`/`subagent`/`all` → `AgentMode`: `subagent` hidden from the agent picker (`ListAvailable` filters server-side). **Visibility-only gating — primary presets remain spawnable**, matching the earlier ruling |
| `steps` | per-preset `AgentPreset::steps`, a CEILING on tool rounds when spawned as a subagent (`subagent_manager` clamps: request may lower, never raise) |
| `temperature`, `top_p`, passthrough extras | EVERY unknown frontmatter key lowers YAML→JSON into `extra_parameters` and merges OVER the resolved model's parameters at resolve time |
| `disable` | `Resolve` throws ("agent preset 'x' is disabled"); listing skips |
| `hidden`, `color` | hidden = listing filter; color rides `AgentPresetSummary` (new proto field) into the picker via a new optional `MenuRow::color` slot |
| deprecated `tools` | `name: true/false` → per-tool allow/deny overrides (the deny-list shape opencode means; minimal's `allowed_tools` is an allow-list and cannot express it) |

As built, the deviation from the sketch: the "listed in spawn_subagent's tool description" half of `mode` is unnecessary in minimal — `spawn_subagent` already accepts ANY preset, so subagent presets are spawnable without being listed (the tool's `agent_preset` parameter is free-form). Bundled additions: `general` and `explore` subagent presets (`bundled/agents/`, both `mode: subagent` with `steps` ceilings). Unknown-key passthrough is deliberate (opencode's "additional options" behavior); a typo'd structural key becomes a harmless unknown parameter rather than a load error — warned about in the header comment, chosen over warning-per-key because passthrough is a feature, not a mistake.

Bundled additions: `general`, `explore` subagent presets. TUI core: agent picker honors `hidden`/`color` (metadata rides the wire; the picker is core frame per EXTENSIBILITY §4.1).

### Phase 3 — Skill gating: allow / ask / deny / preload (server core) — **DONE**

`allowed_skills` keeps its **list form = `allow`** (back-compat) and gains a map form; v1 supports exact names + the existing `*` key (glob patterns ride the deferred pattern track):

```yaml
allowed_skills: { git-release: preload, internal-*: ask, experimental-x: deny }
```

- `allow` — today's behavior (listing bullet + `skill` tool load).
- `preload` — full body appended to `preset.system_prompt` at registry load (`agent_preset_registry.cpp:185`); removed from the tool listing and from tool-side `allowed_skills`.
- `ask` — per-skill permission key `skill:<name>` through the existing Ask machinery.
- `deny` — hidden from the listing entirely; `skill` tool refuses.
- Subagent clamping (`subagent_manager.cpp`): most-restrictive lattice `deny > ask > allow > preload`.
- Validation (as built in `skill_registry.cpp`, warn-not-fail): the **directory name is the identity**; a frontmatter `name`, when present, must match it; a scalar non-empty `description` is required (collapsed to a single line, since it becomes one system-prompt bullet). There is **no name regex and no 1024-char cap** — the opencode-strict validation was not carried over. Unknown frontmatter tolerated.

**Status: complete.** As built: `enum class SkillMode { Preload=0, Allow=1, Ask=2, Deny=3 }` (the ordering IS the clamp lattice). The resolved `AgentPreset::allowed_skills` holds only Preload/Allow/Ask — explicit denies resolve OUT, because absence is deny, which makes the clamp's "parent without the entry" rule uniform. Two structural findings fixed the sketch:

1. **The clamp had to move INSIDE resolve.** A preload entry is consumed at resolve time (its body is baked into the cached system prompt), so the originally-planned post-resolve clamp in `subagent_manager` could never stop the injection — the child's prompt already carried the body. `Resolve(name, root, parent_skills)` now clamps elementwise BEFORE baking (uncached when the parent ceiling is given; spawn is rare), and `subagent_manager` clamps nothing itself.
2. **Ask rides the substrate, not a new prompt path.** One new optional Tool virtual, `mode_override_for(input, ctx)` (narrow-only, folded `most_restrictive` in `ToolExecutor::ResolveMode` BEFORE the run mode — Bypass still washes it to allow, AllowOnly still clamps to deny). SkillTool narrows ask-mode skills to `Ask` and offers the stable grant key `skill:<name>` through the existing `Disclose`/`GrantOffer` machinery, so "always" covers later loads of the same skill and no other. Preloaded skills never reach `ToolContext::allowed_skills` (their bodies are already in the prompt); unreadable preloads demote to `Allow` with a warning instead of silently vanishing.

### Phase 4 — `commands-md` plugin (server plugin) — **DONE**

One bundled server plugin; all template logic stays plugin-local.

**Discovery:** `<config>/commands/`, `<project>/commands/`, `<project>/.opencode/commands/`; project shadows global (warn); style-mixed files allowed + lint-warned.

**Model — body is inert, execution is frontmatter-only:**

```yaml
---
description: Does something with {x}
args: [x, y]
optional: [aaa]
sources:
  abc:
    run: ["/usr/local/bin/gen.py", "--fast", "--x", "{x}"]  # argv; {name} per-entry; missing optional → entry omitted
    # sh: "git log | head"    # XOR with run; /bin/sh -c; opencode desugaring target
    output: {type: json, fields: [summary, steps]}  # json → body renders {output:abc.field}; text (default)
    timeout: 600             # seconds; default 600; 0 = infinite
    output_cap: {max: 50000, on_exceed: confirm}    # token estimate (bytes/4); settings-level defaults
body: |
  Review {x} against {output:abc.summary}, given: {log}
---
```

- Sources are flat and independent, evaluated in declaration order; no cross-source references.
- Body renders `{name}`, `{output:src.field}`, `{args}` in a single pass; **inserted values are never re-scanned**. No executors in the body.
- No `body` → the prompt is the sole text source's stdout (error if multiple sources).

**Opencode desugaring (parse-time):** `$ARGUMENTS`→`{args}`, `$1..$n`→args-list positionals, `` !`cmd` ``→anonymous `sh` source in appearance order. **`@file` expansion is NOT implemented** — `commands_md_parse.cpp` desugars only the three constructs above; the planned body-level expansion never shipped (the inert-body ruling removed the place it would have lived) and remains a known gap.

**Subtask semantics:** `agent: <subagent>` (default) or `subtask: true` (any preset, primary included; parent ceilings apply) → `spawn_subagent` invocation (`task` plus optional `agent_preset`/`blocking`/`max_turns` — the tool has **no `model` parameter**, so a frontmatter `model:` cannot be forwarded; bare `model:` warns-unsupported v1).

**Failure & gating:** generator nonzero exit / timeout / missing `{output:field}` → `CommandResult{kError}` → ephemeral error status line; **no turn, no transcript entry** (test-pinned). Cap exceed → `confirm` default: a per-invocation ConfirmationBroker prompt (`submit` / `cancel`) — there is **no session-scoped "always" grant** (`commands-md:cap:<command>` never shipped; the code asks every time) and **no rlimits** are applied. `on_exceed: fail` available per source. Executed program+argv visible in the tools panel; generators run with a filtered env allowlist as in hooks/bash.

**Registration:** each file → `PromptShorthandCommand` via `minimal_plugin_register_commands`; autocomplete and kind annotation free via `ListCommands`; renaming via `plugins.json` `aliases`.

**Status: complete.** As built (`plugins/server/commands-md/`, a bundled plugin like any other): `commands_md_parse.cpp` owns loading/desugaring, `commands_md.cpp` the command class and a fork/exec runner (two pipes + poll, process-group SIGKILL on timeout and on cap, stderr kept as the error narrative, output trailing-newline trimmed). Three findings refined the sketch:

1. **Substitution is LENIENT everywhere as built.** Source argv/scripts substitute KNOWN names only and pass unknown braces through verbatim — a JSON payload or awk program in a `sh:` string is ordinary shell syntax, not a mistyped parameter. The body render started strict (it threw on unknown `{...}`, on the theory a typo must fail loudly, never submit a hole) but shipped lenient instead: unknown tokens pass through as literal text, with a one-time warn for identifier-shaped tokens (`{agr}` warns; `{"type": ...}` stays quiet), so brace-bearing prose — showing the model a JSON example — remains usable. The loud-failure paths survive where typos cannot be the cause: missing required args, a failed source, an empty composed prompt.
2. **Unfilled optionals bind as empty for the template** (render nothing) and drive the argv-omission rule (the whole entry is dropped — what "optional" means on a command line). A shell script referencing an unfilled optional errors (a script cannot omit pieces).
3. **Shadowing is decided by STEM before parsing** — a malformed project command does not fall back to the global command the author was overriding; it is skipped with a warning like any other broken file.

`model:`/`agent:` frontmatter now ride the per-turn override (below, §6 — landed); `subtask:` still warns-unsupported. The opencode-desugared path ships with a lint warning when a file mixes declared sources with `!`cmd`` constructs.

### Phase 5 — `checklist-view` TUI plugin — **DONE (segment first)**

Bundled client plugin: panel/view + `{todo}` status segment via the ClientPlugin ABI; polls `checklist.rpc` on turn events through `ClientApi` (EXTENSIBILITY §2.5 "poll first" ruling) — zero server changes. Aliases `todowrite`/`todoread` → checklist ops in bundled `plugins.json`.

**Status: complete as the `{todo}` status segment, and (post-plan) the `checklist.panel` view.** (`plugins/client/checklist-view/`, on the cost-view shape: background poll of `checklist.rpc`/`get` for the focused session, immediate refresh on tab switch, empty/missing checklist renders as an unfilled segment, degraded registration when no client_api seam). Bundled `ui.json` now shows `{todo}` by default. The dedicated PANEL/VIEW deviation (1) has since resolved: the plugin ships a toggleable Panel view (`checklist.panel`, default `ctrl+k`) with two surfaces over the same shared poller — FOCUSED (the current tab's tasklist) and SUMMARY (per-tab done/total counts, display names via the new `FocusProvider` client seam) — still poll-based; `PluginSubscribe` push remains deferred for it, stated plainly. (2) the `todowrite`/`todoread` aliases do not map: minimal's checklist is ONE tool with ops, not two tools, and aliases rename tools, not ops.

### Phase 6 — Provider config compat (server core + provider plugins) — **DONE**

- Alias map in `providers_config.cpp`: `options.baseURL`→`url`, `options.apiKey`→`api_key`, `options.headers`→new headers passthrough (both provider plugins honor it), `models.<id>.name`→display name, `limit.context`→`context_window_tokens`, `limit.output`→`max_tokens`, `models.<id>.options`→`parameters` (per-key fold, native `parameters` keys win — opencode's AI SDK spreads a model's `options` into the request body, which is exactly what minimal's `parameters` passthrough does; e.g. OpenRouter's `{"options": {"provider": {"only": ["z-ai/fp8"]}}}}` routes verbatim). `npm` ignored with a warning (`@ai-sdk/openai-compatible` ⇒ `backend: "openai"`). Static model keys may contain `/` (split at the first), so OpenRouter's `vendor/model` ids are expressible as hand-written static entries — the key is the wire `model` verbatim and the full ref is `<provider>/vendor/model`.
- `blacklist`/`whitelist` → catalog filter applied core-side on the static ∪ discovered union (provider plugins unchanged).
- `auth-file` plugin (bundled, may ship disabled): AuthProvider-seam implementation reading opencode-shaped `~/.local/share/opencode/auth.json` for keys absent from `providers.json` -- gated behind a per-entry `"credentials": "auth-file"` opt-in (see correction 3 below).
- `/connect` command (in the auth plugin): paste key → `<data dir>/auth.json` (opencode-shaped, minimal-owned); core never writes credentials.
- `small_model` (`settings.json`) → consumed by `core.session_support` (titler already resolves overrides; extended to the core compaction summarizer).

**Status: complete, with two seam corrections recorded:**

1. **auth.json reading does NOT use the AuthProvider seam** — that seam answers "who may CONNECT" (a per-RPC allow/deny policy), not "what key does provider X use"; the plan's original placement was a misreading. As built, the api-key chain in `ResolveApiKey` gained a third link: `<data dir>/auth.json`, then opencode's own `~/.local/share/opencode/auth.json` (zero-copy compat), both `{"<provider>": {"key": "..."}}`, missing-file-normal / malformed-warns. `/connect <provider> <key>` is a sessionless command in commands-core that atomically merges into that same file — core never writes config, and the credentials file lives outside the seeded config dir. Keys resolve ONCE at parse time and each entry's resolution is logged at startup (source named, key value never); an entry that opted into `auth-file` but resolved nothing warns then, so a key written after the server started (e.g. via `/connect`) announces itself instead of surfacing only as a 401 on the first request.
2. **`small_model` rides LoopConfig, not session_support** — the compaction summarizer is invoked inside AgentLoop with the session's own provider; the honest redirect is a resolved `LoopConfig::SmallModel` (provider + model name + parameters + cap), stamped once into the registry's loop template from `settings.small_model` at startup. Unset/unresolvable = compact on each session's own model (warn, never fatal). This only covers COMPACTION; the titler has its own plugin-config override.
3. **The auth.json fallback requires an explicit per-entry opt-in: `"credentials": "auth-file"`.** Without it, an entry with neither `api_key` nor `api_key_env` resolves an empty key -- it does NOT fall through to `<data dir>/auth.json` or opencode's own store. The fallback decouples "who names the request URL" from "who holds the secret": a `providers.json` entry pasted from an untrusted source, redirecting `url`/`options.baseURL` to an attacker's host and supplying no key of its own, would otherwise silently pull the operator's real key out of the shared auth store and send it wherever `url` points, with nothing in `providers.json` that looks like a credential at all. This was found and closed in code review before the phase's "zero-copy" claim shipped to users; the fix is one added line per opted-in entry, e.g. `{"anthropic": {"credentials": "auth-file", ...}}`.

The alias layer normalizes each provider entry in place BEFORE the strict parse (native keys win on collision; `models.<id>.name` is dropped — the picker shows ref ids; a known unknown `npm` package warns instead of silently choosing nothing). `blacklist`/`whitelist` filter `ModelRefs()` (the picker's source) only — an explicit resolve of a filtered model still works, because a filter is a listing concern, not an access rule. Headers ride `llm::ProviderConfig::headers` and both bundled chat providers append them after their own required headers.

**Sequencing:** 1 → 2 → 3 → 4 → 5 → 6; Phase 5 parallelizable any time (client-only); Phase 4 depends on Phase 1 and touches Phase 2's role registry. Docs updated per phase: `CONFIGURATION.md`, `EXTENSIBILITY.md`, README command table.

## 4. Out of scope by construction

- **Plugin compatibility.** opencode plugins are TS/Bun; minimal's are native C++ shared libraries. No bridge. (A sidecar or MCP shim would be the only path; not planned.)
- **A parallel config format.** Aliases only; minimal's multi-file config stays authoritative.

## 5. Verification posture

Each phase ships with Catch2 tests beside the existing per-feature files (`agent_preset_frontmatter_test.cpp`, config/registry tests, `client_plugins_test.cpp` fixtures, headless E2E where wire changes occur); `make test` + lint per `BUILD.md`. Pinned invariants: results of `/`-commands never enter the chat; a child preset never widens skill modes; a mixed-style command file and a desugared opencode file behave identically to their pure forms.

## 6. Explicitly deferred (recorded, not scheduled)

| Item | Note |
|---|---|
| ~~Permission glob-pattern rules~~ | **landed** as `pattern_overrides` / `external_directory_patterns` in permissions.json (plus the same maps in preset frontmatter): most-restrictive fold over matching rules replaces the category default — `{"git *": allow, "git push --force *": ask}` works, order-independently. `.env` read-denial ships as a built-in default pattern set. Deliberate deviation: no last-match-wins (the substrate folds `most_restrictive`; a strict-default-plus-specific-allow pair is inexpressible), and untrusted project-local presets contribute no pattern rules |
| ~~Per-turn model/agent override on `SubmitTurn`~~ | **landed**: `UserChatTurn` carries optional `model`/`agent_preset`; the worker borrows for one turn (restore on every exit path; the borrowed preset's run mode may narrow, never widen), unlocking command frontmatter `agent:`/`model:`. `spawn_subagent` also gained a `model` parameter — a fresh child context is the cheap way to switch models, vs the full-history cache miss an in-session borrow pays. |
| `.rpc`-addressable computed templates | md placeholders calling any plugin's `.rpc` op (`{{checklist list}}`); makes `.rpc` payloads a de-facto cross-plugin API — needs a stability note first |
| Typed `TemplateDataSource` registry | new ABI symbol + NameRegistry kind; only if computed templates become an ecosystem pattern |
| ~~Session fork for commands~~ | **landed (store + registry + wire)**: `ForkSession` RPC forks a session's active transcript (whole, or below a text-bearing user message) into a new top-level peer session, with optional model/preset/run-mode overrides and a `forked_from_session_id` lineage column (v7). Compaction checkpoints ride along when they cover the copied prefix; the source is untouched. Still open: md-command `fork:` frontmatter -- it needs a session→registry seam (a live forked session cannot be constructed from inside a session worker), deferred rather than forced |
| MCP resources + prompts, McpObserver seam | designed in EXTENSIBILITY §2.4, waits there |
| Edit snapshots / file-undo (`/undo` reverting tool effects) | conversation rewind/redo exists; snapshot journal is the missing half |
| Share/export | local-first analogue: export-to-markdown/HTML command; share server is a bigger decision |
| LSP + formatters | natural fit: server plugin + `post_tool_use` hook |
| TUI diff rendering | renderer *slot* exists (`registries.hpp`), no implementation |
| `@`-mention subagent invocation | prompt-editor autocomplete listing agent presets → `spawn_subagent` shorthand |
| Models.dev-style catalog plugin | fits `ModelCatalogRegistry`; local-first design may prefer `discover: true` |
| `PluginSubscribe` push consumer | the RPC is implemented server-side (dispatches to a plugin's `.endpoint`); no bundled plugin ships a push source yet — checklist still polls |
