# checklist

The model's per-session task checklist: a `checklist` tool plus an RPC service
serving the same state to UI consumers.

## What it adds

- **Tool**: `checklist` — the model writes its copy of the list (create /
  update / complete items). The tool result text is what the MODEL sees;
  presentation is deliberately not server-rendered.
- **RPC service**: `checklist.rpc` — serves the same state as JSON over the
  generic PluginCall channel, the data path a web or mobile frontend would
  call.

## State

- One list per session, in the plugin's storage namespace, keyed by session id.
- Survives session termination: a resumed session keeps its list, and rows are
  deliberately NOT purged on session end (there is no session-deleted signal,
  and purging would break resume).
- Degrades gracefully: on a host without the storage seam the tool answers
  every op with a clear error and the service replies `!ok` — a runtime
  condition, not a crash.

## Configuration

None.
