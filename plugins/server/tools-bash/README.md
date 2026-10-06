# tools-bash

The `bash` tool (synchronous and background), the `/restart-shell` and `/tasks`
commands, the per-session shell-state service, and a session-lifecycle
observer. Split from tools-fs because it has a distinct security profile and is
the one tool with a plausible alternate implementation (container / remote
executor) someone would actually swap: disable this plugin and ship one
registering `bash`.

## What it adds

- **Tool**: `bash` — commands run through `/bin/sh -c` (operator-swappable via
  `shell`). Category Execute, Ask by default: the permission gate IS the
  boundary; there is no allowlist. Invocations disclose the binaries they name,
  by resolved path, and the operator may grant them for the session. Config-level
  `pattern_overrides` for bash match per **stage** (see docs/CONFIGURATION.md):
  an allow rule must cover every stage of the command line, ask/deny rules
  match any stage.
- **Background bash**: the same command string through the same gate, but the
  call returns immediately. The model-facing flag is `detach=true`
  (`run_in_background` stays as an accepted alias). Tasks are killable
  (`kill -- -PID` from a later call) and killed when the session ends; a
  process-global SIGKILL sweep at server exit is the last-resort backstop.

### Long-running tasks

Pass `detach: true` (never `nohup ... &`): the call acks at once with a task
id and pid, the result is injected at the next turn boundary. Inside a
detached task, redirect output to a file to follow it mid-run. A bare
`nohup cmd &` in a FOREGROUND call is the one trap: the grandchild inherits
the output pipe, so the call wedges to its deadline. The drain fails fast on
that shape — once the shell exits, a pipe still open with no data for
`kIdleAfterReapMs` (2 s, bash_process.hpp) ends the run and the result
carries a note pointing at `detach` or full redirection
(`cmd > log 2>&1 < /dev/null &`, the deliberate escape hatch that releases
the pipe and outlives the session).
- **Commands**: `/restart-shell` (resets the session's cwd/env), `/tasks`
  (lists background tasks).
- **Hook point**: `tools-bash.before_command` — a veto-capable point fired
  before the shell spawns, filterable by `program`; a Block means this plugin
  refuses the invocation.
- **Service**: `BashSessionStateService`, the sanctioned channel other plugins
  use to reach the per-session cwd/env store.

## Configuration

The plugin's own file, `plugins.d/<name>.json` (one per plugin, beside `plugins.json`), flat keys. Every
field is optional; non-positive values fall back to defaults.

`plugins.d/tools-bash.json`:

```json
{
  "timeout_s": 120,
  "max_timeout_s": 600,
  "background_max_timeout_s": 600,
  "max_background_tasks": 4,
  "max_output_bytes": 1048576,
  "max_file_size_bytes": 1073741824,
  "shell": "/bin/sh",
  "inherit_env": "allowlist"
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `timeout_s` | `120` | Wall-clock cap per invocation, seconds; the whole process group is killed at the deadline. The model-facing `timeout` tool input uses the same unit. |
| `max_timeout_s` | `600` | Ceiling on the model-supplied `timeout`. A model may ask for longer than the default (a build is not `ls`) but not without bound. |
| `background_max_timeout_s` | `600` | Detached tasks' own ceiling — generous, because a long-running build is the point of the mode. A task with no `timeout` gets this ceiling itself. |
| `max_background_tasks` | `4` | Per-session cap on live background tasks; over the limit is an error result, not a queue. |
| `max_output_bytes` | `1048576` (1 MiB) | Bounds the PERSISTED result. Live streaming is uncapped (lossy by design); this is the record. |
| `max_file_size_bytes` | `1073741824` (1 GiB) | RLIMIT_FSIZE backstop for written files. |
| `shell` | `/bin/sh` | A different POSIX shell, or a wrapper that enters a container, needs no code change. |
| `inherit_env` | `allowlist` | How the session's shell environment is seeded. `allowlist` filters to `$HOME`, `$PATH`, `$TMPDIR`, ... (the server process env carries provider API keys; seeding all of it let `env` persist credentials into the transcript). `all` restores the full seed as an explicit operator choice. Any other value is a named startup error. |

`max_timeout_s` below `timeout_s` (and `background_max_timeout_s` below
`timeout_s`) is raised to `timeout_s`: configuring a *maximum* must not
silently shorten every command that did not ask for a timeout.

## Notes

- Per-session cwd/env maps and the background registry live INSIDE this plugin
  (core holds no bash state) and are released on session end.
