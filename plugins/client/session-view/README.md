# session-view

The session and subagent pickers.

## What it adds

- **View** `sessions` (modal): this project's stored sessions, newest first,
  with a `+ new session` row. Arrow-down past the last row loads the next
  page; `tab` sorts by recency or name; `ctrl+a` lists every project's;
  `ctrl+d` deletes the highlighted one after a confirmation (Escape cancels
  it). Enter opens a session -- attached when it is still live, resumed
  from its transcript otherwise (`HostActions::OpenSession` / `NewTab`).
- **View** `subagents` (modal): the focused session's subagents, live first;
  `ctrl+a` shows the ended ones too. Enter focuses an open tab, attaches a
  live one into its parent's tab group, or opens an ended one read-only
  (`HostActions::OpenSubagent`).

The ids are roles: core opens `sessions` for `ctrl+o` and a bare `/sessions`
on a tab without a session, and the server's `/sessions` and `/subagents`
results name these views in their view hints. Lists and deletes go through
`ClientApi` (`ListSessions`, `ListSubagents`, `DeleteSession`).

## Configuration

None.
