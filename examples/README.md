# Examples

Copyable starting points for everything configuration-shipped: agent presets,
system prompt fragments, skills, and markdown `/`-commands. **None of these
directories is a discovery path** — nothing here is loaded as-is. Copy what
you want into your config dir (`$XDG_CONFIG_HOME/minimal`, default
`~/.config/minimal`) or a project directory, then restart the server.

| Directory | Holds | Copy into |
|---|---|---|
| [agents/](agents/) | agent presets (frontmatter + system prompt) | `agents/` (global) or `<project>/agents/` |
| [prompts/](prompts/) | system prompt fragments | `prompts/` (global only) |
| [skills/](skills/) | one directory per skill, each with a `SKILL.md` | `skills/` (global) or `<project>/skills/`, `.claude/skills/`, `.agents/skills/` |
| [commands/](commands/) | markdown `/`-commands | `commands/` (global) or `<project>/commands/`, `.opencode/commands/` |

Each subdirectory's README lists every file with the feature it demonstrates
and the copy commands. Walkthroughs: `docs/CONFIGURATION.md` (agents,
prompts, skills), `docs/COMMANDS_HOWTO.md` (commands). Project-local copies
shadow global ones of the same name — the nearest declaration wins.
