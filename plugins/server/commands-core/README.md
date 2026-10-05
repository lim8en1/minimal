# commands-core

The built-in `/-commands`, as one plugin.

## What it adds

| Command | Usage | What it does |
| --- | --- | --- |
| `/help` | `/help [command]` | Lists all registered commands (including plugins loaded after this one), or explains one. Read-only; answers even on a sessionless tab. |
| `/compact` | `/compact [prompt]` | Summarizes the older half of the conversation into one message. The transcript is kept. With a name, uses `prompts/<name>.md` as the summarization prompt for this compaction only (auto-compaction and bare `/compact` keep the active preset's prompt). |
| `/clear` | `/clear` | Forgets the conversation so far (asks for confirmation). The transcript on disk is unchanged — what the model sees and what scrollback shows are different things. |
| `/agent` | `/agent [name]` | Bare form opens the agent-preset picker; with a name, switches the session's preset. |
| `/model` | `/model [provider/model]` | Bare form opens the model picker; with a `<provider>/<model>` reference, switches the session's model. The choice is recorded on the session, so a later `/agent` re-applies it rather than resolving it away. |
| `/sessions` | `/sessions` | Opens the session picker. Read-only; needs no session. |
| `/subagent-model` | `/subagent-model [provider/model\|inherit]` | Bare form opens the picker; with a reference, pins the model this session's spawned subagents run on; `inherit` clears it. |
| `/think` | `/think [variant\|off]` | Bare form lists the current model's reasoning-effort variants; with a name, switches to `<model>@<name>`; `off` returns to the base model. If a `/model` choice is in force the variant replaces it (and survives `/agent`); otherwise only the current preset's model changes, and a later `/agent` uses the new preset's own model. |
| `/subagents` | `/subagents` | Opens the subagent picker for this session's children. Read-only. |
| `/rename` | `/rename <name>` | Renames this session (the raw tail, spaces allowed); an explicit name is never overwritten by the titler. |
| `/mcp` | `/mcp [name enable\|disable]` | Bare form opens the MCP picker; with a name, enables/disables that server live, without a restart. Needs no session. |
| `/permissions` | `/permissions [revoke <key>\|all] [check <tool> <json-input>]` | Shows this session's effective permission rules and grants; revokes grants; dry-runs a call through the gate. |
| `/secret` | `/secret set <name> <value> [--host <host[:port]>]...` · `list` · `delete <name>` | Manages the named secrets in the data dir's `secrets.json` (mode 0600), referenced from any key field as `secret:<name>` (docs/CONFIGURATION.md § Secrets). `--host` binds where the value may be sent. Takes effect on the next request; `list` never shows values. |

## Configuration

None. The plugin registers its commands at startup and nothing else.

## Notes

- `/restart-shell` is deliberately NOT here: it resets tools-bash's per-session
  state, so it belongs to tools-bash. Command placement follows ownership of
  the state a command touches.
- `/agent` precedence rule: a preset supplies a default model, but a model the
  user chose (via `/model`) is a decision and survives the switch — switching
  presets never silently undoes `/model`.
