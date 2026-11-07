# minimal

An extensible, plugin-based agent harness: the program around a model that
runs the turn loop, sessions, tools and provider calls. It is general-purpose
-- what an agent can do, and what it looks like while doing it, is decided by
the plugins you load, not by the core.

The core is deliberately small. It owns the mechanisms: the turn loop,
sessions and their SQLite store, the permission gate, compaction triggering,
subagents, and the gRPC protocol between server and client. Everything else
is a plugin built against the same ABI a third-party plugin uses -- the tools,
the model providers, the `/`-commands, memory, cost tracking, and on the
client side even the chat surface, the prompt editor and the widget kit.
Disable what you don't want, rename what you keep, replace any of it, add
your own.

The bundled set happens to make a capable coding agent (file, search, shell
and Python tools, coding-oriented agent presets), but that is one
configuration, not the product: swap the tools and presets and the same
harness runs a research agent, an ops assistant, or something else.

- **Plugin-first.** ~20 server extension points (tools, dynamic tool sources
  such as MCP, providers and model catalogs, commands, agent preset
  extensions, context contributors, compaction strategies, permission models
  and advisors, hook points, cross-plugin services) and ~15 client ones
  (views, renderers, status segments, the chat surface, prompt editor and
  widget kit). Each plugin owns its config file, its permission policy and
  its slice of the UI.
- **Tweakable all the way down.** Most of what is a hard-coded choice
  elsewhere is configuration here:
  - Compaction is a pluggable strategy, chosen globally or per agent preset.
    Its instructions are plain prompt files you edit (`prompts/compression.md`,
    `prompts/compression-structured.md`).
  - The system and subagent prompts are markdown files in the config dir, and
    so are the agent presets.
  - Tools can be renamed through aliases, or replaced outright: disable the
    bundled plugin and load your own of the same name.
  - Most capabilities can be switched off: a whole plugin in `plugins.json`,
    a single tool per agent preset (`allowed_tools`), or refused everywhere
    by a `deny` rule in `permissions.json`.
- **Light.** A native C++ server and client. The running server uses tens of
  MiB of RAM (about 25 MiB typical). Built for long conversations: 512k–1M
  token histories without holding or re-rendering full history; compaction is
  triggered from context occupancy and streamed incrementally.
- **Local and single-user.** One server per machine, shared by every client;
  sessions persist in SQLite and survive client disconnects. Clients are a
  C++/FTXUI terminal UI and a headless one-turn mode (`minimal -p`) over the
  same gRPC interface.

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
| `-p, --print PROMPT` | headless one turn; a prompt starting with `/` runs a command; `-p -` reads the prompt from stdin |
| `--resume SESSION_ID` | open a stored session: attach to it in the TUI (or run one headless turn with `-p`); it keeps its stored preset and project_root |
| `--list-sessions` | list stored sessions for the current project (`--all-dirs` for every project); `--output-format json` prints the same list for scripts |
| `--server TARGET` | socket path or gRPC target (default `unix://<data dir>/server.sock`) |
| `--agent PRESET` | agent preset (file stem of `agents/<name>.md`); with `--resume` it applies to the resumed turn only (requires `-p`) |
| `--model P/M` | model as `<provider>/<model>` (overrides preset and settings) |
| `--output-format text\|jsonl\|result` | headless output format; `result` prints only the turn's structured result (a node preset's `submit_result`) and exits 6 without one (`json` is a `--list-sessions` listing) |
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
and never overwritten afterward (`--target update-config` refreshes the files
you never edited; see BUILD.md).

| File | Status | Purpose |
|---|---|---|
| `settings.json` | required | compaction, tools, subagents, project context, logging, session/server lifetimes |
| `providers.json` | required | LLM backends and models (addressed as `<provider>/<model>`) |
| `permissions.json` | required | permission fallbacks per tool category, per-tool overrides |
| `hooks.json` | optional | shell-command hooks at named points (veto via exit code 2) |
| `mcps.json` | optional | MCP servers (stdio/http) |
| `cost-rates.json` | optional | cost-tracker's fallback price table |
| `channels.json` | optional | external signal channels |
| `plugins.json` | optional | plugin enable/disable, aliases |
| `plugins.d/<name>.json` | optional | one plugin's own settings |
| `agents/*.md` | ≥ 1 required | agent presets (frontmatter + markdown) |
| `prompts/*.md` | optional | system prompt fragments |
| `keybindings.json`, `ui.json`, `themes/` | optional (client) | keymap, status bar, theme |

Full reference — every field, type, default, and allowed value:
**[docs/CONFIGURATION.md](docs/CONFIGURATION.md)** · Copyable example agents,
prompts, skills, and commands: **[examples/](examples/)**.

## Plugins

What ships in the box, every piece of it removable or replaceable:

| | Server plugins |
|---|---|
| Tools | `tools-fs` (read/write/edit), `tools-search` (grep/glob), `tools-bash`, `tools-python` (persistent interpreter), `tools-web` (fetch/search), `tool-ask` (structured questions to the user), `checklist`, `signals-message` (cross-session messaging), `mcp` (MCP servers as dynamic tools) |
| Models | `provider-openai`, `provider-anthropic` |
| Agent behavior | `skills`, `memory`, `project-context` (AGENTS.md/CLAUDE.md), `compaction-structured`, `node` (pipeline nodes: JSON input/output contracts for a preset), `decision-advisor`\*, `session-titler`\* |
| Commands | `commands-core` (the built-in `/`-commands), `commands-md` (markdown `/`-commands) |
| Other | `cost-tracker`, `opencode-compat` (reads an existing opencode setup) |

| | Client plugins |
|---|---|
| The TUI itself | `chat-view`, `chat-renderers`, `prompt-editor`, `ui-kit`, `markdown-md4c`, `syntax-grammars`, `tab-bar`, `session-status`, `consent-prompts` |
| Views and pickers | `tools-panel`, `session-view`, `selection-view`, `config-view`, `theme-view`, `help-view`, `session-export`, `logo` |
| Halves of server plugins | `ask-view`, `checklist-view`, `cost-view`, `mcp-view` |

\* Ships disabled. `decision-advisor` sends tool input to a decision-model
endpoint to auto-answer permission prompts, so enabling it takes three
deliberate edits (see its [README](plugins/server/decision-advisor/README.md)).

Drop your own into `<config>/plugins/`, enable/disable any plugin in
`plugins.json` (`client-plugins.json` for the client), configure it in
`plugins.d/<name>.json`, and rename tools/commands via `plugins.json`'s
`"aliases"` map — see **[docs/PLUGINS.md](docs/PLUGINS.md)**; the full
extension surface is in **[docs/EXTENSIBILITY.md](docs/EXTENSIBILITY.md)**.

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
please open an issue first to discuss the approach. Commits need a DCO
sign-off (`git commit -s`) -- see [CONTRIBUTING.md](CONTRIBUTING.md).
