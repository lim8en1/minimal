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
  | `options.apiKey` | `api_key` (unless set) |
  | `options.headers` | `headers` (unless set) |
  | `models.<id>.limit.context` / `.output` | `context_window_tokens` / `max_tokens` |
  | `models.<id>.options` | folded into `parameters` (native keys win per key) |
  | `models.<id>.name` | dropped |

- **Secret source.** `~/.local/share/opencode/auth.json` is the
  `opencode-compat` secret namespace: any key field may name
  `"secret:opencode-compat:<provider-id>"` (read per use, so an opencode
  login reaches the next request). Nothing reads it implicitly.

## Configuration

None.

## Disabled

opencode-shaped `providers.json` entries fail core's parse with a named error
(no `url`/`backend`), and `secret:opencode-compat:...` references fail at use
(no loaded plugin provides them). Command files
under `.opencode/commands/` are commands-md's, not this plugin's.
