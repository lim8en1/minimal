# session-status

The status bar's usual segments.

## What it adds

Status segments, each rendered from the focused tab's state the host hands
every segment (`plugins::TabStatus`):

| id | shows | gives way |
|---|---|---|
| `{permission_mode}` | the run mode; a staged change as `→ mode`; red for bypass | last |
| `{activity}` | thinking / running `<tool>` (with a spinner) / waiting / error / cancelling, tinted by state | |
| `{model}`, `{agent}` | what the session runs with; a staged choice as `→ value` | |
| `{tokens}` | context use, `62k/200k · 31%`, tinted as the window fills | |
| `{paused}` | `paused`, when the agent is suspended | |
| `{session}` | the session's name; the one value the bar may cut short | |
| `{version}` | the build | first |

Each segment places itself (left: `version` 2, `session` 4, `activity` 10,
`tokens` 20; right: `model` 10, `agent` 20, `permission_mode` 30, `paused`
40). `ui.json`'s `status_segments` moves or hides any of them
(docs/CONFIGURATION.md "The status bar"); `version` and `session` drop first
when the bar is too narrow.

## Configuration

None.
