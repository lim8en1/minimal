# tools-web

`web_fetch` and `web_search`. One plugin, two tools: they share the HTTP
client, settings parsing, URL policy, and truncation path.

## What it adds

- **Tools**: `web_fetch` (URL → text, with redirects and truncation) and
  `web_search` (one configurable JSON search API).
- Both declare `network_access_required()`, a permission dimension independent
  of Execute — allowing Execute for a sandboxed bash does not thereby allow
  fetching arbitrary URLs.
- `web_search` is inert and self-describing when unconfigured: the tool still
  registers and reports that it needs `search_api_url`, rather than failing as
  misconfigured.

## Configuration

The plugin's own `config` object in its `plugins.json` entry, one sub-object
per tool. Every field is optional.

```json
{
  "file": "tools-web.so",
  "enabled": true,
  "config": {
    "fetch": {
      "timeout_ms": 15000,
      "max_redirects": 5,
      "allow_private_hosts": false
    },
    "search": {
      "api_url": "http://localhost:8888/search",
      "api_key_env": "SEARXNG_API_KEY",
      "default_max_results": 5,
      "max_results": 20,
      "timeout_ms": 10000
    }
  }
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `fetch.timeout_ms` | `15000` | Hard wall-clock bound on a fetch. |
| `fetch.max_redirects` | `5` | Redirect follow limit. |
| `fetch.allow_private_hosts` | `false` | Loopback/private/link-local hosts (including the cloud-metadata address `169.254.169.254`) are refused by default — the URL comes from the MODEL. Set true only for a trusted internal network. |
| `search.api_url` | *(empty = unconfigured)* | Queried as `GET <url>?q=<query>&count=<n>`; must return `{"results": [{"title", "url", "snippet"}, ...]}`. SearXNG's JSON API is close to this shape; a vendor whose API differs needs a thin adapter in front of it. |
| `search.api_key_env` | *(unset)* | Environment variable resolved once at startup and sent as `Authorization: Bearer <key>` when non-empty. Naming an UNSET variable is a fail-loud startup error; a key that resolves to empty is fine (a local SearXNG needs none). |
| `search.default_max_results` | `5` | Result count when the model supplies none. |
| `search.max_results` | `20` | Absolute ceiling. |
| `search.timeout_ms` | `10000` | Hard wall-clock bound. |

Result size needs no tool-local cap beyond the HTTP client's own 8 MiB
buffered-response limit; the agent loop caps every tool's persisted result at
one central chokepoint.
