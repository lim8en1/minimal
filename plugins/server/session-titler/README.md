# session-titler

Names a session after its first turn, via the LLM. The second layer of session
naming: the free default (first user text, truncated) is always there; this
plugin replaces it with a generated title once, asynchronously, after turn 1.

## What it adds

- **SessionObserver**: after the session's first turn completes, generates a
  title from the first user message and renames the session. One title per
  session, ever, per process. The database is authoritative; the plugin holds
  no session state beyond a one-shot guard.
- Titles are cleaned to a tab-label shape (trimmed, unquoted, whitespace
  collapsed, capped at 60 code points). Empty model output keeps the default.

## Configuration

The plugin's own `config` object in its `plugins.json` entry. Parsed leniently:
a malformed or missing value falls back to the default — a bad number in an
optional cosmetic plugin must not disable it.

```json
{
  "file": "session-titler.so",
  "enabled": true,
  "config": {
    "model": "openrouter/openai/gpt-5-mini",
    "max_tokens": 64,
    "input_char_budget": 2000,
    "prompt": ""
  }
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `model` | *(empty = the session's own model)* | `<provider>/<model>` override. The default costs no second connection. Set a small model to spend less. |
| `max_tokens` | `64` | Max tokens for the title request. |
| `input_char_budget` | `2000` | The first user message is truncated to this before it is sent (a prompt can carry inlined @-mention file content; only enough to title by is needed). |
| `prompt` | *(empty = built-in instruction)* | Overrides the default titling instruction. |

## Enabled state

Ships **DISABLED** (`session-titler` is in the DefaultsDisabled list): it is
the one bundled plugin whose entire job is to spend the user's tokens on its
own initiative (~100 per session). Trusted code, but a cost the operator
should opt into — enable it with one `plugins.json` edit (`"enabled": true`).
