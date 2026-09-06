# commands-core

The built-in `/-commands`, as one plugin rather than nine.

## What it adds

| Command | Usage | What it does |
| --- | --- | --- |
| `/help` | `/help [command]` | Lists all registered commands (including plugins loaded after this one), or explains one. Read-only; answers even on a sessionless tab. |
| `/compact` | `/compact` | Summarizes the older half of the conversation into one message. The transcript is kept. |
| `/clear` | `/clear` | Forgets the conversation so far (asks for confirmation). The transcript on disk is unchanged — what the model sees and what scrollback shows are different things. |
| `/agent` | `/agent [name]` | Bare form opens the agent-preset picker; with a name, switches the session's preset. |
| `/model` | `/model [provider/model]` | Bare form opens the model picker; with a `<provider>/<model>` reference, switches the session's model. The choice is recorded on the session, so a later `/agent` re-applies it rather than resolving it away. |
| `/sessions` | `/sessions` | Opens the session picker. Read-only; needs no session. |
| `/connect` | `/connect <provider> <api-key>` | Stores a credential in the data dir's `auth.json` (mode 0600), used when the provider's providers.json entry names no key. Takes effect after a server restart. |

## Configuration

None. The plugin registers its commands at startup and nothing else.

## Notes

- `/restart-shell` is deliberately NOT here: it resets tools-bash's per-session
  state, so it belongs to tools-bash. Command placement follows ownership of
  the state a command touches.
- `/agent` precedence rule: a preset supplies a default model, but a model the
  user chose (via `/model`) is a decision and survives the switch — switching
  presets never silently undoes `/model`.
