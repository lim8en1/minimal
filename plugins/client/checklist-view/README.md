# checklist-view

The client-side UI for the [checklist](../../server/checklist/) server plugin's
data: two presentation surfaces over one fetch path.

## What it provides

- **`{todo}` status segment** — `todo 2/5 (1 active)` for the focused
  session; unfilled while the list is empty or not yet known; ` (stale)`
  appended while the server connection is down. Placed at the left side's
  end (left 50), after cost-view's segments.
- **`checklist.panel` view** — a toggleable panel (default binding
  `alt+l`, rebindable under that id in `keybindings.json`) with two
  surfaces, flipped with `Tab` while the panel has focus:
  - **This tab** — the focused session's tasklist (`[x]` done, `→` in
    progress, `[ ]` pending) with a `N/M done` footer; long lists scroll to
    the item in progress.
  - **All tabs** — one line per open tab with tasks (`display_name  d/t`,
    plus an active count), the focused tab marked. Display names come from
    the client's focus seam; closed tabs and tabs without tasks are not
    listed.

The panel is strictly read-only: the model owns checklist mutations through
the `checklist` tool; a user edit here would race it.

## How the data moves

One background worker (`ChecklistFeed`) owns every call to the server
plugin. It holds the `changes` push stream and caches each session's list;
both surfaces read the cache and nothing ever calls out from a render path
(the `StatusSegment::Render` cheap/never-block contract). Open tabs the
stream has not pushed yet are seeded with one filtered `list` call, so a tab
switch shows that tab's list at once.

If the stream is unavailable (an older server, a restart, transport loss),
cached lists are marked stale and the open tabs are polled with `list` every
3 s; the stream is retried after 30 s, backing off to 5 min while it keeps
failing.

Degrades, never crashes: no `client_api` seam → the plugin registers
nothing; no `focus` seam → the panel view is not registered (the segment
still runs, keyed by the frame's focused session id).
