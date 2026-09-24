# cost-view

The client-side status segments for the [cost-tracker](../../server/cost-tracker/)
server plugin's data.

## What it provides

- **`{cost}`** — `$1.23 · 45.6k/7.8k tok` for the focused session, subagent
  subtree included. An unknown price renders `cost?`, and a partly priced
  total renders as a floor (`>= $1.23`). It never shows `$0.00` for "unknown".
- **`{token_detail}`** — `i 45.6k · o 7.8k · cr 1.2M · cw 3.4k`. Zero
  fields are left out.

Both segments are unfilled until the session has spent something.

## Data path

A single background worker (`totals_feed.hpp`) holds one
`PluginSubscribe("cost-tracker", "totals")` stream. The server pushes a
session's totals whenever a provider round, a turn or a rewind changes them,
so the figures move during a turn. Results are cached per session, which
means switching to a tab you have already seen shows its figures straight
away. A tab the stream hasn't pushed since this client subscribed gets
filled by one `PluginCall`, at most one server heartbeat (500 ms) after you
switch to it.

If the stream fails or ends (server restart, transport loss, or a server
without the endpoint), the cached figures stay on screen with a ` (stale)`
suffix. The worker then polls the focused session every 3 s for 30 s and
tries to subscribe again. A server that only offers `cost-tracker.rpc`
keeps working through the polls.

## Files

- `cost_view_format.hpp` — pure formatters (header-only, tested in
  `tests/cost_view_format_test.cpp`).
- `totals_feed.{hpp,cpp}` — the stream, cache and fallback (tested in
  `tests/cost_view_feed_test.cpp`).
- `cost_view_plugin.cpp` — registration only.
