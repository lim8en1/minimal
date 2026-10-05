# tools-web

`web_fetch` and `web_search`. One plugin, two tools: they share the HTTP
client, settings parsing, URL policy, and truncation path.

## What it adds

- **Tools**: `web_fetch` (URL → text, with redirects and truncation) and
  `web_search` (one configurable JSON search API).
- Both are `read_only` and declare `network_access_required()`: the `network`
  permission (default `ask`) is their gate. Allowing Execute for a sandboxed
  bash does not allow fetching URLs, and `network: allow` does not allow bash.
- **A permission model** (per-host rules, below) that refines `network` for
  these two tools.
- `web_search` is inert and self-describing when unconfigured: the tool still
  registers and reports that it needs `search_api_url`, rather than failing as
  misconfigured.

## Configuration

The plugin's own `config` object in its `plugins.json` entry, one sub-object
per tool. Every field is optional; an unknown field (top level, `fetch` or
`search`) keeps the plugin from loading, naming the field.

```json
{
  "file": "tools-web.so",
  "enabled": true,
  "config": {
    "fetch": {
      "timeout_ms": 15000,
      "max_redirects": 5,
      "default_max_bytes": 262144,
      "max_bytes": 2097152,
      "allow_private_hosts": false
    },
    "search": {
      "api_url": "http://localhost:8888/search",
      "api_key": "secret:searxng",
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
| `fetch.default_max_bytes` | `262144` (256 KiB) | Text returned when the model supplies no `max_bytes`. |
| `fetch.max_bytes` | `2097152` (2 MiB) | Absolute ceiling on the model-supplied `max_bytes`; a ceiling below the default is raised, not honoured. |
| `fetch.allow_private_hosts` | `false` | Loopback/private/link-local hosts (including the cloud-metadata address `169.254.169.254`) are refused by default — the URL comes from the MODEL. Set true only for a trusted internal network. |
| `search.api_url` | *(empty = unconfigured)* | Queried as `GET <url>?q=<query>&count=<n>`; must return `{"results": [{"title", "url", "snippet"}, ...]}`. SearXNG's JSON API is close to this shape; a vendor whose API differs needs a thin adapter in front of it. |
| `search.api_key` | *(unset)* | A key reference (docs/CONFIGURATION.md § Secrets: `"secret:NAME"`, `"env:NAME"`, or a literal), sent as `Authorization: Bearer <key>`. A stored secret is read per search; an `"env:"` variable that is UNSET is a fail-loud startup error. Unset = no header (a local SearXNG needs none). |
| `search.default_max_results` | `5` | Result count when the model supplies none. |
| `search.max_results` | `20` | Absolute ceiling. |
| `search.timeout_ms` | `10000` | Hard wall-clock bound. |

### Per-host permissions

The plugin's permission model replaces core's single `network` mode for
`web_fetch` (judged by its URL's host) and `web_search` (judged by the search
backend's host). Configure it in `permissions.json`, and per preset:

```json
{ "plugins": { "tools-web": {
    "default": "ask",
    "hosts": { "*.internal": "deny", "*.example.com": "ask", "api.example.com": "allow" } } } }
```

```yaml
plugin_permissions:
  tools-web: { hosts: { "docs.python.org": allow } }
```

- Globs match the whole host, case-insensitively; the most specific matching
  glob wins (ties go to the stricter mode). A preset's rule replaces the
  operator's for the same glob.
- An unmatched host takes `default`, else core's `network` mode -- with no
  slice at all, nothing changes.
- A preset that sets `network` itself (plan's `network: deny`) is not widened
  by the operator's host rules; only the preset's own host rules can.
- A project-local preset may only narrow.
- Core still applies everything else: per-tool denies, subagent ceilings, run
  modes, grants, the prompt.
- `web_fetch` no longer follows a redirect to a *different host* (a decision
  made for one host must not carry to another): it returns the target URL, and
  fetching it is its own, gated call. Same-host hops still follow.

### Result size

The HTTP client buffers at most 8 MiB and the agent loop caps every tool's
persisted result at one central chokepoint — but a response that fits both can
still be far more text than a turn needs, so `web_fetch` bounds its own return:
the model gets the first `max_bytes` bytes (default `fetch.default_max_bytes`,
ceiling `fetch.max_bytes`, `max_bytes: 0` means the ceiling), and everything
over that is spilled to `<project_root>/.minimal/web-fetch/<url-hash>.txt` —
a path the footer names, page-able with the `read` tool. Spill files are a
cache: one per distinct URL, overwritten on refetch, safe to delete. If the
spill write itself fails, the truncation still happens and the footer says the
pointer is missing instead of returning one that leads nowhere.
