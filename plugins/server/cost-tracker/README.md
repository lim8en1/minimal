# cost-tracker

Per-session token usage and estimated cost. COST IS DERIVED, NOT REQUESTED —
nothing in the agent loop or the provider changes; a SessionObserver records
every completed turn (the only writer).

## What it adds

- **SessionObserver**: records every completed turn (model, usage) per session.
- **Command**: `/cost [session-id|all]` — renders a session's (or every
  session's) totals as text. Read-only.
- **Command**: `/cost-update-rates [url]` — opt-in live refresh of the LiteLLM
  pricing snapshot. No network at startup, none during a turn.
- **RPC service**: `cost-tracker.rpc` — serves the same state as JSON over the
  generic PluginCall channel (the data path a future UI consumes).
- **Seed file**: `cost-rates.json` in the config dir, user-editable text —
  never overwritten, never compiled into the plugin.
- The model has no tool here: cost is the operator's business.

## Pricing

Three tiers, config > fetched > bundled, all resolved through ONE RateTable so
a report can never disagree with itself:

1. The user's `rates` slice in plugins.json (below).
2. The LiteLLM snapshot cached by `/cost-update-rates` (keys are model ids —
   the ccusage convention; a cached snapshot from an earlier run is picked up
   at startup).
3. The seeded `cost-rates.json` in the config dir.

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
    "rates_fetch_timeout_ms": 30000
  }
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `rates` | *(none)* | Per-model rate overrides, consumed verbatim as the RateTable's config tier. Keys are model ids. |
| `rates_fetch_url` | LiteLLM `model_prices_and_context_window.json` on GitHub | What `/cost-update-rates` fetches. A constant by default so the command is useful with zero configuration; overridable for a mirror or pinned revision. |
| `rates_fetch_timeout_ms` | `30000` | The fetch runs on the read-only command pool beside a live turn, so it must be bounded even when the transfer stalls. |

## Notes

- History blobs survive session termination and are NOT purged on session end
  (a resumed session keeps its history; there is no session-deleted signal).
