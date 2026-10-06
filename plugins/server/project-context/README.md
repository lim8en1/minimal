# project-context

The project's own instructions file in every session's context: the first of
`files` found in the session's project root (with `search_ancestors`, also
its ancestors, nearest first), read once per session — creation, resume and
subagent spawn alike — and frozen with the session's context.

## What it adds

A context contributor at **priority 0**, so the section leads the session's
context tail, ahead of signal-channel instructions (50) and other plugins'
sections (100).

## Configuration

`plugins.d/project-context.json`, every key optional:

| Key | Default | Notes |
| --- | --- | --- |
| `files` | `["AGENTS.md", "CLAUDE.md"]` | plain file names, tried in order within each directory |
| `max_bytes` | `262144` | `0` = unlimited; a longer file is cut with a visible marker. The generic `context_contributors.max_bytes_per_section` does not apply |
| `search_ancestors` | `false` | also walk up from the project root, nearest directory first |

A symlinked file resolving outside the directory it was found in is ignored.

## Disabled

No instructions file is read; `context_contributors.disabled: ["project-context"]`
in `settings.json` does the same without disabling the plugin.
