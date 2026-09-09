# provider-openai

The OpenAI chat-completions provider, registered under the kind string
`"openai"`, which providers.json references via a provider entry's `backend`
field. The registry is string-keyed precisely so a plugin can add a kind core
has never heard of.

## What it adds

- **Provider factory**: `backend: "openai"` — the OpenAI `/v1/chat/completions`
  shape.
- **Model catalog**: backends of this kind can list their own models, which
  powers `"discover": true` in providers.json (lazy; the `/model` picker's
  ctrl+r re-fetches on demand; never written back to the file).

Targets any OpenAI-compatible endpoint via `url`, including local llama.cpp /
vLLM servers — which is why llama.cpp needs no dedicated provider.

## Configuration

The plugin itself has no config; it is configured through the provider entries
in providers.json (see `bundled/providers.example.json`):

```json
{
  "providers": {
    "local": {
      "backend": "openai",
      "url": "http://localhost:8000/v1",
      "models": { "qwen4": { "context_window_tokens": 131072 } }
    },
    "hosted": {
      "backend": "openai",
      "url": "https://api.example.com/v1",
      "api_key_env": "MINIMAL_API_KEY",
      "discover": true
    }
  }
}
```

| Field | Meaning |
| --- | --- |
| `url` | The endpoint, WITH its `/v1` suffix (this backend's convention — provider-anthropic's differs). |
| `api_key_env` | Environment variable resolved once at startup; preferred over an inline `api_key` (never both). |
| `discover` | `true` = list the backend's own models; no `models` object needed. Static entries beside it still work, and a model's own window wins over the provider default. |
| `models.<key>` | The KEY is sent on the wire verbatim as the request's `model` field. |

Provider-level values (`max_tokens`, `parameters`, `context_window_tokens`)
are defaults for every model below; a model may override any of them.

## Notes

- Thread-safety contract: instances are shared across concurrent sessions, so
  the implementation holds no mutable state across calls — one CURL handle per
  call, stream-reassembly state local to the call.
