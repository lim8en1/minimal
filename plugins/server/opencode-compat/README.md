# opencode-compat

Reads opencode's configuration so an existing opencode setup works without
copying anything. Core knows only its own config shapes; this plugin is the
one place that knows opencode's.

## What it adds

Through `minimal_plugin_register_provider_config_extensions`:

- **providers.json translator.** An opencode-shaped provider entry is
  rewritten into core's shape before core parses it:

  | opencode | core |
  | --- | --- |
  | `npm: "@ai-sdk/openai-compatible"` | `backend: "openai"` (other packages warn: set `backend`) |
  | `options.baseURL` | `url` (unless set) |
  | `options.apiKey` | `api_key` (unless `api_key`/`api_key_env` set) |
  | `options.headers` | `headers` (unless set) |
  | `models.<id>.limit.context` / `.output` | `context_window_tokens` / `max_tokens` |
  | `models.<id>.options` | folded into `parameters` (native keys win per key) |
  | `models.<id>.name` | dropped |

- **Credential source.** `~/.local/share/opencode/auth.json` is consulted
  after core's own `<data dir>/auth.json`, only for entries that opt in with
  `"credentials": "auth-file"` (the same gate core applies to its own file).

## Configuration

None.

## Disabled

opencode-shaped `providers.json` entries fail core's parse with a named error
(no `url`/`backend`), and opencode's `auth.json` is never read. Command files
under `.opencode/commands/` are commands-md's, not this plugin's.
