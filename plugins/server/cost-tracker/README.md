# cost-tracker

Per-session token usage and estimated cost. COST IS DERIVED, NOT REQUESTED —
nothing in the agent loop or the provider changes; a SessionObserver records
every completed turn (the only writer).

## What it adds

- **SessionObserver**: records every completed turn (model, usage) per session.
- **Command**: `/cost [session-id|all]` — renders a session's (or every
  session's) totals as text. Read-only.
- **Startup auto-refresh**: when the cached pricing snapshot is older than
  `rates_refresh_days` (default 7), the plugin fetches fresh LiteLLM +
  OpenRouter prices on a bounded background thread as the server boots. No
  network during a turn; a failed refresh keeps the previous prices.
- **Server flag**: `minimal-server --refresh-cost-rates` — fetch both pricing
  snapshots now and exit (bypasses the staleness gate). The manual escape
  hatch the old `/cost-update-rates` command was; there is no TUI command for
  refreshing prices.
- **RPC service**: `cost-tracker.rpc` — serves the same state as JSON over the
  generic PluginCall channel (the data path a future UI consumes); its
  `refresh_rates` method is what the server flag drives.
- **Seed file**: `cost-rates.json` in the config dir, user-editable text —
  never overwritten, never compiled into the plugin.
- The model has no tool here: cost is the operator's business.

## Pricing

Four tiers, config > openrouter-fetched > litellm-fetched > bundled, all
resolved through ONE RateTable so a report can never disagree with itself:

1. The user's `rates` slice in plugins.json (below).
2. The fetched OpenRouter snapshot, cached by the startup auto-refresh, the
   `--refresh-cost-rates` flag, or the `.rpc` service's `refresh_rates` method
   (keys are model ids — the ccusage convention; a cached snapshot from an
   earlier run is picked up at startup). Ranked above the LiteLLM snapshot
   because the wire ids an OpenRouter-shaped provider sends ARE OpenRouter
   slugs, so OpenRouter's own price for that exact string is authoritative;
   LiteLLM's `openrouter/*` entries duplicate the same data and lag it.
3. The fetched LiteLLM snapshot, the same paths, the same rule.
4. The seeded `cost-rates.json` in the config dir.

A model with no rate anywhere — and a turn whose usage the backend reported as
all zeros (the known proxy gap) — renders as "unknown", never as $0.00.

## Configuration

The plugin's own `config` object in its `plugins.json` entry. Every field is
optional; an absent or partial config object is normal rather than an error.

```json
{
  "file": "cost-tracker.so",
  "enabled": true,
  "config": {
    "rates": {
      "claude-opus-5": { "input": 15.0, "output": 75.0, "cache_read": 1.5 }
    },
    "rates_fetch_url": "https://raw.githubusercontent.com/BerriAI/litellm/main/model_prices_and_context_window.json",
    "rates_fetch_timeout_ms": 30000,
    "rates_refresh_days": 7
  }
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `rates` | *(none)* | Per-model rate overrides, consumed verbatim as the RateTable's config tier. Keys are model ids. |
| `rates_fetch_url` | LiteLLM `model_prices_and_context_window.json` on GitHub | What the fetch (startup auto-refresh, the flag, the `.rpc` method) reads. A constant by default so pricing works with zero configuration; overridable for a mirror or pinned revision. |
| `rates_fetch_timeout_ms` | `30000` | Every fetch — the startup auto-refresh included — is bounded even when the transfer stalls. |
| `rates_refresh_days` | `7` | How stale the cached snapshot may be before a server start fetches again. `0` fetches on every start; the `--refresh-cost-rates` flag always fetches regardless. |

## Notes

- History blobs survive session termination and are NOT purged on session end
  (a resumed session keeps its history; there is no session-deleted signal).
- The auto-refresh worker is joined at server teardown (via the observer
  registry's drain, before plugin storage is destroyed), so an in-flight
  startup fetch can extend exit by up to roughly `rates_fetch_timeout_ms` —
  the same discipline as the discovery warm-up's exit join.
