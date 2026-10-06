# minimal

A coding-agent harness — the program that runs the LLM turn loop, tools,
sessions, and provider calls around a model — built to be light on CPU and RAM
and to be taken apart: nearly everything is a plugin you can reconfigure,
rename, replace, or remove.

It is a C++ **server** that owns the turn loop, tools, sessions, and provider
dispatch, and a C++/FTXUI **terminal client** that renders it — plus a
headless one-turn mode (`minimal -p`) over the same gRPC interface.

- **Light.** A native C++ server and client. The running server uses about
  25 MiB of RAM, versus roughly 1 GiB per opencode instance and ~300 MiB for Claude
  Code, at about a quarter of opencode's CPU usage (informal measurements on
  the author's machine). Built for long conversations: 512k–1M token histories
  without holding or re-rendering full history; compaction is triggered from
  context occupancy and streamed incrementally.
- **Plugin-first.** Read/write/edit/bash, the model providers, and even the
  built-in `/`-commands are bundled plugins using the same ABI a third-party
  plugin would. Core owns mechanisms, you own the config: disable what you
  don't want, alias what you do, add your own.
- **Local and single-user.** One server per machine, one binary per client;
  sessions persist in SQLite and survive client disconnects.

Architecture and design decisions: [ARCHITECTURE.md](ARCHITECTURE.md) ·
Extensibility: [docs/EXTENSIBILITY.md](docs/EXTENSIBILITY.md) ·
Licensed under [Apache 2.0](LICENSE).

## Quick start

Requires a C++20 compiler on macOS or Linux (native Windows is not supported;
use WSL).

```sh
export VCPKG_ROOT=/path/to/vcpkg
make            # configure + build (cmake preset `default`)
make install    # binaries, libraries, plugins + config seeding
```

Then point `providers.json` at a backend and run:

```sh
minimal         # TUI client (auto-spawns the local server if needed)
```

Full build/install instructions (presets, sanitizers, install layout, config
seeding): **[BUILD.md](BUILD.md)**.

## Command line

`minimal` — TUI client and headless client in one binary:

| Flag | Description |
|---|---|
| `-p, --print PROMPT` | headless one turn; a prompt starting with `/` runs a command |
| `--resume SESSION_ID` | open a stored session: attach to it in the TUI (or run one headless turn with `-p`); it keeps its stored preset and project_root |
| `--list-sessions` | list stored sessions for the current project (`--all-dirs` for every project); `--output-format json` prints the same list for scripts |
| `--server TARGET` | socket path or gRPC target (default `unix://<data dir>/server.sock`) |
| `--agent PRESET` | agent preset (file stem of `agents/<name>.md`); with `--resume` it applies to the resumed turn only (requires `-p`) |
| `--model P/M` | model as `<provider>/<model>` (overrides preset and settings) |
| `--output-format text\|jsonl` | headless output format (`json` is a `--list-sessions` listing) |
| `--signals LIST` | comma-separated signals to receive (requires `-p`) |
| `--dangerously-bypass` | auto-approve permission prompts (configured denies still apply) |
| `--no-autospawn` | never start a server; fail if none is reachable |

A headless run prints the session id it used — `[session <id>]` on stderr in
text mode, a `session_created` line first in `jsonl` — so scripts can chain
turns and open the result in the TUI:

```sh
id=$(minimal -p "refactor the parser" --output-format jsonl | head -1 | jq -r .session_id)
minimal --resume "$id"    # attach to the session (and its live subagents)
```

`minimal-server` — the server: `--listen <path>`, `--plugin-call <plugin>
<method> [<json-args>]` (call a plugin's `.rpc` handler once and exit, printing
the reply — e.g. `--plugin-call cost-tracker refresh_rates` refreshes the cached
price snapshots), `--check` (validate config and plugins, then exit: 0 when the
server would start) and `--version`. Normally spawned for you by the first client
that needs it; it exits on its own once nothing has used it for a while
(sessions stay resumable for `sessions.detached_idle_ttl_seconds`, default 30 min).

## Configuration

Everything lives in one config directory (`$MINIMAL_CONFIG_DIR` →
`$XDG_CONFIG_HOME/minimal` → `$HOME/.config/minimal`), seeded at install time
and never overwritten afterward.

| File | Status | Purpose |
|---|---|---|
| `settings.json` | required | compaction, tools, subagents, project context, logging, session/server lifetimes |
| `providers.json` | required | LLM backends and models (addressed as `<provider>/<model>`) |
| `permissions.json` | required | permission fallbacks per tool category, per-tool overrides |
| `hooks.json` | optional | shell-command hooks at named points (veto via exit code 2) |
| `mcps.json` | optional | MCP servers (stdio/http) |
| `cost-rates.json` | optional | cost-tracker's fallback price table |
| `channels.json` | optional | external signal channels |
| `plugins.json` | optional | plugin enable/disable, per-plugin config, aliases |
| `agents/*.md` | ≥ 1 required | agent presets (frontmatter + markdown) |
| `prompts/*.md` | optional | system prompt fragments |
| `keybindings.json`, `ui.json`, `themes/` | optional (client) | keymap, status bar, theme |

Full reference — every field, type, default, and allowed value:
**[docs/CONFIGURATION.md](docs/CONFIGURATION.md)** · Copyable example agents,
prompts, skills, and commands: **[examples/](examples/)**.

## Plugins

Everything interesting ships as a plugin: `tools-fs`, `tools-search`,
`tools-bash`, `tools-python`, `tools-web`, `tool-ask`, `provider-openai`,
`provider-anthropic`, `commands-core`, `commands-md`, `checklist`,
`signals-message`, `cost-tracker`, `memory`, `session-titler`,
`decision-advisor` (server), and `logo`, `cost-view`, `checklist-view`
(client). `session-titler` and `decision-advisor` ship disabled;
`decision-advisor` sends tool input to a decision-model endpoint to auto-answer
permission prompts, so enabling it takes two deliberate edits (see its
[README](plugins/server/decision-advisor/README.md)). Drop your own
into `<config>/plugins/`, enable/disable or reconfigure any plugin in
`plugins.json`, and rename tools/commands via its `"aliases"` map — see
**[docs/PLUGINS.md](docs/PLUGINS.md)**.

Markdown-defined `/`-commands work zero-copy with opencode's layout: put
`.md` files in the project's `commands/` or `.opencode/commands/`, or the
config dir's `commands/` — walkthrough in
**[docs/COMMANDS_HOWTO.md](docs/COMMANDS_HOWTO.md)**, reference in
**[docs/CONFIGURATION.md](docs/CONFIGURATION.md)**.

## Development

```sh
make build-tests   # build including test binaries (make build skips them)
make test          # build tests + ctest (Catch2)
make asan          # AddressSanitizer build (dependencies instrumented too)
make tsan          # ThreadSanitizer build
make server-only   # no TUI client
```

## Security and contributing

Report vulnerabilities privately as described in [SECURITY.md](SECURITY.md).
Issues and pull requests are welcome; for anything larger than a small fix,
please open an issue first to discuss the approach.
