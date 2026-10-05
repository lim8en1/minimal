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
      "api_key": "env:MINIMAL_API_KEY",
      "discover": true
    }
  }
}
```

| Field | Meaning |
| --- | --- |
| `url` | The endpoint, WITH its `/v1` suffix (this backend's convention — provider-anthropic's differs). |
| `api_key` | A key reference: `"secret:NAME"` (stored with `/secret set`, read per request — no restart), `"env:NAME"` (read at startup; unset is an error), or a literal. See docs/CONFIGURATION.md § Secrets. |
| `discover` | `true` = list the backend's own models; no `models` object needed. Static entries beside it still work, and a model's own window wins over the provider default. |
| `models.<key>` | The KEY is sent on the wire verbatim as the request's `model` field. |

Provider-level values (`max_tokens`, `parameters`, `context_window_tokens`)
are defaults for every model below; a model may override any of them.

### Session routing (`session_id`)

The harness session id is sent as a top-level `session_id` body field on every
request by default (agent turns, subagents, and the compaction summarizer
alike). Backends that use it for sticky routing — OpenRouter routes the whole
session to the provider endpoint whose prompt cache is warm, and groups the
requests in its Logs "Sessions" view — benefit automatically; backends that
ignore unknown body fields are unaffected. To turn it off (e.g. an
OpenAI-compatible backend that rejects unknown fields), set
`"parameters": { "session_id": false }` at provider or model level; a STRING
`session_id` in `parameters` passes through verbatim as a static routing key
instead of the per-session one. See `bundled/providers.example.json`.

## Notes

- Thread-safety contract: instances are shared across concurrent sessions, so
  the implementation holds no mutable state across calls — one CURL handle per
  call, stream-reassembly state local to the call.
