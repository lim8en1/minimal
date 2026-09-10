# checklist-view

The client-side UI for the [checklist](../../server/checklist/) server plugin's
data: two presentation surfaces over one fetch path.

## What it provides

- **`{todo}` status segment** — `todo 2/5 (1 active)` for the focused
  session; unfilled while the list is empty or missing.
- **`checklist.panel` view** — a toggleable panel (default binding
  `ctrl+k`, rebindable under that id in `keybindings.json`) with two
  surfaces, flipped with `Tab` while the panel has focus:
  - **This tab** — the focused session's tasklist (`[x]` done, `→` in
    progress, `[ ]` pending) with a `N/M done` footer.
  - **All tabs** — one line per open tab (`display_name  d/t`, plus an
    active count), the focused tab marked. Display names come from the
    client's focus seam; sessions whose tab is closed are not listed.

The panel is strictly read-only: the model owns checklist mutations through
the `checklist` tool; a user edit here would race it.

## How the data moves

One background worker (`ChecklistPoller`) owns every `checklist.rpc` call —
`get` for the focused session, `list` for all sessions — and caches the
answers. Both surfaces read the cache; nothing ever calls the RPC from a
render path (the `StatusSegment::Render` cheap/never-block contract). A
focus change clears the focused-session cache and wakes the worker for an
immediate refetch, so a tab switch never shows the previous tab's tasks.

Degrades, never crashes: no `client_api` seam → the plugin registers
nothing; no `focus` seam → the panel view is not registered (the segment
still runs, nudged by the frame's per-render focused session id); a failed
fetch renders the last-known data with a stale marker.
