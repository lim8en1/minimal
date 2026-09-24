# checklist

The model's per-session task checklist: a `checklist` tool plus an endpoint
serving the same state to UI consumers, pushed as it changes.

## What it adds

- **Tool**: `checklist` — the model writes its copy of the list (`set` /
  `update` / `view`). The tool result text is what the MODEL sees, a numbered
  tasklist whose numbers are the ids `update` takes; presentation is
  deliberately not server-rendered. Calls run sequentially within a session,
  so a `set` and an `update` in one turn apply in the order written.
- **Endpoint**: `checklist.endpoint` — the generic PluginCall /
  PluginSubscribe channel a TUI, web or mobile frontend uses:
  - `get {"session_id"}` → `{"session_id", "items":[{id,text,status}]}`
  - `list {"session_ids":[...]}` → `{"checklists":[{session_id, items}]}` —
    only the named sessions that have a list. Omit `session_ids` to list every
    stored session (grows without bound; prefer the filter).
  - subscribe `changes` → one `{"type":"checklist","session_id","items"}` push
    per session after every write (`items: []` = cleared), and
    `{"type":"heartbeat"}` every 500 ms while idle.
- **RPC service**: `checklist.rpc` — the same `get` / `list`, for consumers
  that resolve it directly.

## State

- One list per session, in the plugin's storage namespace, keyed by session id.
- Survives session termination: a resumed session keeps its list, and rows are
  deliberately NOT purged on session end (there is no session-deleted signal,
  and purging would break resume).
- A stored row that fails to decode is reported loudly; `op=set` replaces it.
- Degrades gracefully: on a host without the storage seam the tool answers
  every op with a clear error and every call is refused — a runtime
  condition, not a crash.

## Configuration

None.
