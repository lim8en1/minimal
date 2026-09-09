# tools-python

The persistent Python interpreter: the `python_exec`, `python_output`,
`python_input`, and `python_interrupt` tools, the `/python` command, and a
session-lifecycle observer.

## What it adds

- **Tool**: `python_exec` — code runs in a PERSISTENT per-session interpreter
  (state survives across calls). Category Execute, Ask by default; executed
  code runs with the full privileges of the local user, and disclosure names
  the resolved interpreter binary. Same "model error, not malice" threat model
  as bash: against an adversarial model, only a container helps.
- **Tool**: `python_output` — poll background work without blocking: task
  listing (running/queued/finished) and byte-offset slices of a running
  task's live stdout stream (or a finished task's final capture), with an
  optional post-scope line regex.
- **Tool**: `python_input` — feed the stdin channel of the running code; a
  blocking `input()` parks the interpreter until this is called.
- **Tool**: `python_interrupt` — sends SIGINT (or `kill=true` for the group).
- **Command**: `/python` — inspect or restart the session's interpreter.
- **Lifetime contract**: unlike background bash, nothing SYNCHRONOUS ever
  kills the interpreter — it is shared session state. A sync wait reaching
  `sync_wait_ceiling` DEMOTES to async instead of killing anything; kills are
  always explicit (interrupt with kill, `/python restart`, session terminate,
  server exit). The one destructive timer is the request's own opt-in
  `max_runtime`: on expiry the request is interrupted, and one that ignores
  SIGINT for `grace_seconds` is hard-killed (state lost, by the caller's
  explicit deadline).
- **Environment contract**: state is per-session; the environment is ONE shared
  `.venv` per project, created on demand. Agent presets declare package
  contracts via `tool_config.python` frontmatter (see below); the declared set
  is diffed against the venv on every call — additive entries auto-install
  (immediately importable), spec conflicts are reported loudly instead of
  silently re-versioning the shared environment.

## Configuration

The plugin's own `config` object in its `plugins.json` entry. ALL times are
SECONDS. Every field is optional; non-positive values fall back to defaults
(`sync_wait_ceiling`, `rlimit_cpu_seconds`, and `background_max_runtime` clamp
to 0, which means disabled/unlimited).

```json
{
  "file": "tools-python.so",
  "enabled": true,
  "config": {
    "python": "python3",
    "sync_wait_ceiling": 300,
    "startup_timeout": 10,
    "install_timeout": 300,
    "grace_seconds": 2,
    "max_output_bytes": 1048576,
    "max_capture_bytes": 1048576,
    "rlimit_cpu_seconds": 0,
    "max_file_size_bytes": 1073741824,
    "stream_head_bytes": 262144,
    "stream_tail_bytes": 786432,
    "background_max_runtime": 0
  }
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `python` | `python3` | Fallback interpreter (bare-python path and venv creation); resolved on PATH at use time. |
| `sync_wait_ceiling` | `300` | Ceiling on a model-supplied sync `timeout`. 0 = unlimited. Reaching the ceiling demotes the wait to async — it never destroys state. |
| `startup_timeout` | `10` | Spawn + handshake window for a new interpreter (a broken venv is caught here, not mid-use). |
| `install_timeout` | `300` | Bounded venv creation / package installation. |
| `grace_seconds` | `2` | How long `python_interrupt` (and a `max_runtime` expiry) waits before reporting "no reaction" / escalating to the hard kill. |
| `max_output_bytes` | `1048576` (1 MiB) | Bounds the persisted result text per request. |
| `max_capture_bytes` | `1048576` (1 MiB) | Driver-side per-stream (stdout/stderr) capture cap; bounds the protocol line size. |
| `rlimit_cpu_seconds` | `0` (off) | RLIMIT_CPU backstop for the interpreter process. Off by default: the interpreter's clock is the session's clock, and a CPU-spinning live session is the model's/user's to interrupt. |
| `max_file_size_bytes` | `1073741824` (1 GiB) | RLIMIT_FSIZE backstop for files the executed code writes. |
| `stream_head_bytes` | `262144` (256 KiB) | Live-output ring, head window (first bytes); `python_output` reads this while a task runs. |
| `stream_tail_bytes` | `786432` (768 KiB) | Live-output ring, tail window (last bytes). Total live ring bound is head + tail; bytes outside both come back as explicit gap markers. |
| `background_max_runtime` | `0` (off) | Default `max_runtime` deadline for background requests; a per-call value overrides it. |

## Preset-declared packages

Agent presets (agents/*.md) may declare a package contract in their
`tool_config.python` frontmatter; it rides every invocation as
`ToolContext::tool_config` and is diffed against the shared venv:

```json
{
  "packages": ["impacket", "pefile==2023.2.7"],
  "requirements": ["requirements.txt"]
}
```

- `packages`: package specs, verbatim.
- `requirements`: requirements files, project_root-relative paths.
