# cost-tracker

Per-session token usage and estimated cost. COST IS DERIVED, NOT REQUESTED —
nothing in the agent loop or the provider changes; a SessionObserver records
every completed turn, and every billed call outside a turn's rounds (the
only writer).

## What it adds

- **SessionObserver**: records every completed turn (model, usage) per session,
  and each compaction's summarizer call as its own record (`OnBilledCall`) at
  its model's rate -- the small_model's when one serves compaction. A rewind
  keeps compaction records: the call happened.
- **Command**: `/cost [session-id|all]` — renders a session's (or every
  session's) totals as text. Read-only.
- **Startup auto-refresh**: when any cached pricing snapshot is older than
  `rates_refresh_days` (default 7), the plugin fetches fresh prices from every
  `rate_sources` entry on a bounded background thread as the server boots. No
  network during a turn; a failed refresh keeps the previous prices. A start
  that skips the fetch says so: `pricing snapshots are fresh (oldest fetched
  …, refreshed every N day(s)); not refreshing at startup`.
  A source added to `rate_sources` has no snapshot yet, so the next start
  fetches; one removed or renamed has its cached snapshot deleted at that
  start (`dropped the cached prices of …`) -- its prices no longer count.
- **Server flag**: `minimal-server --plugin-call cost-tracker refresh_rates` — fetch every pricing
  snapshot now and exit (bypasses the staleness gate). The manual escape
  hatch the old `/cost-update-rates` command was; there is no TUI command for
  refreshing prices.
- **RPC service**: `cost-tracker.rpc` — serves the same state as JSON over the
  generic PluginCall channel; its `refresh_rates` method is what the server
  flag drives.
- **Endpoint**: `cost-tracker.endpoint` — the UI data path
  ([cost-view](../../client/cost-view/)). `PluginCall` answers the `.rpc`
  methods; `PluginSubscribe("totals")` streams `{"type":"totals", ...}` for
  every session whose totals change (and for each of its ancestors, since a
  subagent's spend rolls up), plus a `{"type":"heartbeat"}` every 500 ms
  while idle. `totals` loads only the requested subagent subtree: an
  in-memory parent index, built once, replaces a scan of every stored
  session.
- **Seed file**: `cost-rates.json` in the config dir, user-editable text —
  never overwritten, never compiled into the plugin.
- The model has no tool here: cost is the operator's business.

## Pricing

Tiers, config > each fetched source in `rate_sources` order > bundled, all
resolved through ONE RateTable so a report can never disagree with itself:

1. The user's `rates` in `plugins.d/cost-tracker.json` (below).
2. One fetched snapshot per `rate_sources` entry, first entry first, each
   cached under its own name by the startup auto-refresh, the
   `--plugin-call cost-tracker refresh_rates` flag, or the `.rpc` service's
   `refresh_rates` method (keys are model ids — the ccusage convention; a
   cached snapshot from an earlier run is picked up at startup). The default
   list is OpenRouter, then LiteLLM: the wire ids an OpenRouter-shaped
   provider sends ARE OpenRouter slugs, so OpenRouter's own price for that
   exact string is authoritative; LiteLLM's `openrouter/*` entries duplicate
   the same data and lag it. A gateway's own `/model/info` belongs first — it
   prices the exact ids it serves, which no public snapshot knows.
3. The seeded `cost-rates.json` in the config dir.

Source formats:

| `format` | Shape | Model id | Kept rows |
| --- | --- | --- | --- |
| `litellm` | LiteLLM's `model_prices_and_context_window.json`: an object of `*_cost_per_token` entries | the key | input and output priced |
| `openrouter` | OpenRouter's `/api/v1/models`: `data[].pricing` with decimal-string prices | `id` | input and output priced |
| `litellm-proxy` | A LiteLLM proxy's `/model/info`: `data[].model_info` with the `litellm` fields | `model_name` | input priced (output/cache default to 0 — internal gateways serve free models) |

A model with no rate anywhere — and a turn whose usage the backend reported as
all zeros (the known proxy gap) — renders as "unknown", never as $0.00.

## Configuration

The plugin's own file, `plugins.d/<name>.json` (one per plugin, beside `plugins.json`). Every field is
optional; an absent or partial config object is normal rather than an error.

`plugins.d/cost-tracker.json`:

```json
{
  "rates": {
    "claude-opus-5": { "input": 15.0, "output": 75.0, "cache_read": 1.5 }
  },
  "rate_sources": [
    { "format": "litellm-proxy", "url": "https://gw.example/model/info", "api_key": "secret:gateway" },
    { "format": "openrouter", "url": "https://openrouter.ai/api/v1/models" },
    { "format": "litellm", "url": "https://raw.githubusercontent.com/BerriAI/litellm/main/model_prices_and_context_window.json" }
  ],
  "rates_fetch_timeout_ms": 30000,
  "rates_refresh_days": 7
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `rates` | *(none)* | Per-model rate overrides, the RateTable's config tier. Keys are model ids -- the name the provider sends, not `<provider>/<model>`; each entry takes `input`, `output`, `cache_read`, `cache_write` in $ per million tokens, all optional. A negative or non-number price is refused, naming the entry. Applied live. In `/config` each model is its own row; its id is picked from the server's models. |
| `rate_sources` | OpenRouter `/api/v1/models`, then LiteLLM's catalog on GitHub | What the fetch (startup auto-refresh, the flag, the `.rpc` method) reads, highest priority first. Setting it replaces the whole list; `[]` fetches nothing. Each entry: `format` (required, above), `url` (required), `name` (defaults to `format`; must be unique — it keys the cached snapshot, so two sources of one format need names), `api_key` (optional key reference — `"secret:NAME"`, `"env:NAME"`, `"secret:<plugin>:NAME"` or a literal, docs/CONFIGURATION.md § Secrets — sent as `Authorization: Bearer`; a stored secret is read per fetch). Restart to apply. |
| `rates_fetch_timeout_ms` | `30000` | Every fetch — the startup auto-refresh included — is bounded even when the transfer stalls. |
| `rates_refresh_days` | `7` | How stale a cached snapshot may be before a server start fetches again. `0` fetches on every start; the `--plugin-call cost-tracker refresh_rates` flag always fetches regardless. |

## Notes

- History blobs survive session termination and are NOT purged on session end
  (a resumed session keeps its history; there is no session-deleted signal).
- The auto-refresh worker is joined at server teardown (via the observer
  registry's drain, before plugin storage is destroyed), so an in-flight
  startup fetch can extend exit by up to roughly `rates_fetch_timeout_ms` —
  the same discipline as the discovery warm-up's exit join.

## Config check

`/config` checks this plugin's settings after each save and on `c` (docs/PLUGINS.md, "Config checks"). Each `rate_sources` entry is fetched as the refresh would, without storing anything: it answers, its key is accepted, and the snapshot holds prices in the shape its `format` says ("12 models priced") -- each finding on that entry's `url` or `api_key`.
