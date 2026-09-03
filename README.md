# minimal

Local-first, memory/CPU-efficient coding-agent harness: a C++ **server** that
owns the agent turn loop, tools, sessions, and LLM provider dispatch, and a
C++/FTXUI **terminal client** that renders it — plus a headless one-turn mode
(`minimal -p`) over the same gRPC interface.

- **Local and single-user.** One server per machine, one binary per client;
  sessions persist in SQLite and survive client disconnects.
- **Plugin-first.** Read/write/edit/bash, the model providers, and even the
  built-in `/`-commands are bundled plugins using the same ABI a third-party
  plugin would — core owns mechanisms, you own the config.
- **Built for long conversations.** 512k–1M token histories without holding
  or re-rendering full history; compaction is triggered from context
  occupancy and streamed incrementally.

Architecture and design decisions: [ARCHITECTURE.md](ARCHITECTURE.md) ·
Extensibility: [docs/EXTENSIBILITY.md](docs/EXTENSIBILITY.md) ·
MIT licensed.

## Quick start

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
| `--resume SESSION_ID` | continue a stored session (requires `-p`) |
| `--server TARGET` | socket path or gRPC target (default `unix://<data dir>/server.sock`) |
| `--agent PRESET` | agent preset (file stem of `agents/<name>.md`) |
| `--model P/M` | model as `<provider>/<model>` (overrides preset and settings) |
| `--output-format text\|jsonl` | headless output format |
| `--signals LIST` | comma-separated signals to receive (requires `-p`) |
| `--dangerously-bypass` | bypass permission gating |

`minimal-server` — the server: `--listen <path>` (socket path) and
`--version`. Normally spawned for you.

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
| `channels.json` | optional | external signal channels |
| `plugins.json` | optional | plugin enable/disable, per-plugin config, aliases |
| `agents/*.md` | ≥ 1 required | agent presets (frontmatter + markdown) |
| `prompts/*.md` | optional | system prompt fragments |
| `keybindings.json`, `ui.json`, `themes/` | optional (client) | keymap, status bar, theme |

Full reference — every field, type, default, and allowed value:
**[docs/CONFIGURATION.md](docs/CONFIGURATION.md)**.

## Plugins

Everything interesting ships as a plugin: `tools-fs`, `tools-search`,
`tools-bash`, `tools-web`, `provider-openai`, `provider-anthropic`,
`commands-core`, `checklist`, `signals-message`, `cost-tracker` (server), and
`logo`, `cost-view` (client). Drop your own into `<config>/plugins/`,
enable/disable or reconfigure any plugin in `plugins.json`, and rename
tools/commands via its `"aliases"` map — see
**[docs/PLUGINS.md](docs/PLUGINS.md)**.

## Development

```sh
make test          # ctest (Catch2)
make asan          # AddressSanitizer build (dependencies instrumented too)
make tsan          # ThreadSanitizer build
make server-only   # no TUI client
```
