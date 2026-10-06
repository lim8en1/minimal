# provider-anthropic

The Anthropic Messages API provider, registered under the kind string
`"anthropic"`, which providers.json references via a provider entry's `backend`
field — no core change, no enum to keep in sync.

## What it adds

- **Provider factory**: `backend: "anthropic"` — the Anthropic `/v1/messages`
  API, with prompt-caching support (`cache_ttl` parameter).
- **Model catalog**: under the same kind, `"discover": true` lists
  `GET /v1/models` (paginated). Every model arrives with its context window
  (`max_input_tokens`), its output cap (`max_tokens`) and its `capabilities`,
  so a discover entry needs no per-model config, and each model gets the
  `@effort` variants the API says it accepts.
- **Variant-params provider**: under the same kind, derives the wire shape
  for one `@effort` variant from the model's reported capabilities (see
  ARCHITECTURE.md, "Core vs. plugin surface"):
  - adaptive thinking → `thinking: {type: "adaptive", display: "summarized"}`
    plus `output_config: {effort: <word>}`, an unsupported word stepping
    down to the nearest lower one the model takes;
  - budget-only thinking (Haiku 4.5 and older) → `thinking: {type:
    "enabled", budget_tokens: N}`, refit to each request's `max_tokens`;
  - `@none` → no thinking field.

  A model discovery never saw is treated as current (adaptive). That shape
  is rejected by budget-only models, so a static (`discover: false`) entry
  for Haiku 4.5 or older must either turn on `discover` or spell its
  variants out in the map form (`{"high": {"thinking": {"type": "enabled",
  "budget_tokens": 16384}}}`). A backend
  with no registered provider here gets core's generic `reasoning_effort`
  shape instead.

## Configuration

The plugin itself has no config; it is configured through the provider entries
in providers.json (see `bundled/providers.example.json`):

```json
{
  "providers": {
    "anthropic": {
      "backend": "anthropic",
      "url": "https://api.anthropic.com",
      "api_key": "secret:anthropic",
      "discover": true,
      "parameters": { "cache_ttl": "5m" }
    }
  }
}
```

That is the whole entry: every model the key can use appears in `/model`
(`anthropic/claude-opus-5-5`, `anthropic/claude-opus-5-5@high`, ...). Static
`models` still work beside it, and their missing window or cap is filled from
the listing.

| Field | Meaning |
| --- | --- |
| `url` | WITHOUT a `/v1` suffix (unlike provider-openai's convention): the canonical value is `https://api.anthropic.com`, and the provider appends `/v1/messages` itself. |
| `api_key` | A key reference: `"secret:NAME"` (stored with `/secret set`, read per request — no restart), `"env:NAME"` (read at startup; unset is an error), or a literal. See docs/CONFIGURATION.md § Secrets. |
| `discover` | `true` = list `/v1/models` for every model's window, output cap and effort ladder. |
| `max_tokens` | The API requires one on every request. With `discover` it comes from the listing; otherwise set it at provider or model level, or every request fails. |
| `parameters.cache_ttl` | `"5m"` (the default; omit it) or `"1h"`, applied to both breakpoints (system and conversation tail) — see ARCHITECTURE.md's prompt-caching notes on when the 1h write premium pays for itself. |
| `parameters.session_id` | Default on: the harness session id rides every request as the `x-session-id` header (OpenRouter's sticky-routing key; the real Anthropic API ignores custom headers and rejects unknown body fields, so the header is the only safe channel). `false` opts out; a string value passes through on the body verbatim as a static routing key. |
| `models.<key>` | The KEY is sent on the wire verbatim as the request's `model` field — it must be the backend's real model id, not your own label. |

## Notes

- Thread-safety contract: same as provider-openai — instances are shared
  across concurrent sessions, so the implementation holds no mutable state
  across calls.
