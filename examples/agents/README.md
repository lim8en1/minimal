# Example agent presets (`agents/*.md`)

Agent presets: markdown with YAML frontmatter, addressed by file stem
(`--agent research`, `/agent research`, or a spawn request's
`agent_preset`). These files are **templates to copy** — this directory is
not a discovery path, so nothing here is loaded as-is.

To use one, copy it into a discovery directory and restart the server:

```sh
mkdir -p ~/.config/minimal/agents
cp research.md ~/.config/minimal/agents/   # global, listed in the picker

# or project-local — resolved BEFORE the config dir, nearest wins:
mkdir -p .agents && cp research.md .agents/../agents/   # <project>/agents/
```

Project presets shadow global ones of the same stem (the shadowing is
logged). The bundled presets (`build`, `plan`, `explore`, `general`, `review`) are
seeded into the config dir at install time and never overwritten; these
examples live beside them once copied.

| File | Demonstrates |
|---|---|
| `reference.md` | **Every structural field, annotated**: `plugins:` groups, `tool_sources` (MCP servers), `plugin_permissions` (skills, mcp, tools-web), `permission_mode` overrides/patterns, `allowed_subagents`, `steps`, `compaction_strategy`, `tool_config`, passthrough parameters. Start here |
| `research.md` | Tool selection for non-coding work (`web_search`/`web_fetch`), `network: allow`, write denied |
| `data-analyst.md` | `tool_config.python` (`packages:` ensured into the shared per-project venv), python tools alongside read-only fs tools |
| `writer.md` | **Fail-closed zero tools**: no `allowed_tools` at all means no core tools; unknown frontmatter keys (`temperature:`) pass through to the provider as request parameters |
| `ctf.md` | The full arsenal (`bash`/`write`/`edit`/python) with `execute`/`file_write: allow`, per-plugin `tool_config` |
| `reviewer.md` | `mode: subagent` (spawnable, hidden from the picker), `steps:` round ceiling, opencode `permission:` alias block |
| `orchestrator.md` | `allowed_subagents` named allow-list (a present list is the exact set; `[]` would withhold `spawn_subagent` entirely) |

Notes:

- `allowed_tools` is **fail-closed**: omitted or empty means NO core tools,
  and `permission_mode` only sets ask/allow/deny for tools the preset already
  admits. `["*"]` (all registered tools) must be written explicitly.
- Any frontmatter key the parser does not recognize rides to the provider as
  a request parameter (`temperature`, `top_p`, `reasoningEffort`, ...). So a
  typo'd or outdated structural key is NOT an error: it silently does nothing
  for the preset and is sent to the provider. MCP servers are
  `tool_sources: {mcp: [...]}` and skills are `plugin_permissions: {skills: ...}`
  (see `reference.md`). The recognized structural keys are in
  `docs/CONFIGURATION.md` under "agents/ and prompts/".
- Presets may pin their own `model: <provider>/<model>` (a `<provider>/<model>`
  ref from `providers.json`, overriding `default_model`); none of these do,
  so they inherit the session's model — see `commands/agent-override.md` for
  the per-turn variant.
- `orchestrator.md` names `reviewer` — preset keys must exist when used, so
  copy that pair together (and `explore`/`general` come from the seeded
  bundled presets).
