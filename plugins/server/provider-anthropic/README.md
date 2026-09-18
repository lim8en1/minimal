# provider-anthropic

The Anthropic Messages API provider, registered under the kind string
`"anthropic"`, which providers.json references via a provider entry's `backend`
field — no core change, no enum to keep in sync.

## What it adds

- **Provider factory**: `backend: "anthropic"` — the Anthropic `/v1/messages`
  API, with prompt-caching support (`cache_ttl` parameter).
- **Variant-params provider**: under the same `"anthropic"` kind, derives the
  `thinking: {budget_tokens: N}` wire shape for a providers.json entry's
  `"variants": ["low", "medium", "high", ...]` effort list (see
  ARCHITECTURE.md, "Core vs. plugin surface"). A backend with no registered
  provider here gets core's generic `reasoning_effort` shape instead.

## Configuration

The plugin itself has no config; it is configured through the provider entries
in providers.json (see `bundled/providers.example.json`):

```json
{
  "providers": {
    "anthropic": {
      "backend": "anthropic",
      "url": "https://api.anthropic.com",
      "api_key_env": "ANTHROPIC_API_KEY",
      "max_tokens": 16000,
      "parameters": { "cache_ttl": "5m" },
      "models": {
        "claude-opus-5": { "context_window_tokens": 1000000 }
      }
    }
  }
}
```

| Field | Meaning |
| --- | --- |
| `url` | WITHOUT a `/v1` suffix (unlike provider-openai's convention): the canonical value is `https://api.anthropic.com`, and the provider appends `/v1/messages` itself. |
| `api_key_env` | Environment variable resolved once at startup; preferred over an inline `api_key` (never both). |
| `max_tokens` | REQUIRED (not optional the way OpenAI treats it): set it at provider or model level, or every request fails. |
| `parameters.cache_ttl` | `"5m"` (the default; omit it) or `"1h"` — see ARCHITECTURE.md's prompt-caching notes on when the 1h write premium pays for itself. |
| `parameters.session_id` | Default on: the harness session id rides every request as the `x-session-id` header (OpenRouter's sticky-routing key; the real Anthropic API ignores custom headers and rejects unknown body fields, so the header is the only safe channel). `false` opts out; a string value passes through on the body verbatim as a static routing key. |
| `models.<key>` | The KEY is sent on the wire verbatim as the request's `model` field — it must be the backend's real model id, not your own label. |

## Notes

- Thread-safety contract: same as provider-openai — instances are shared
  across concurrent sessions, so the implementation holds no mutable state
  across calls.
