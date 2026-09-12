# commands-md

Markdown-defined `/-commands`: drop a `.md` file in a commands directory and it
becomes a `/command`. One plugin, no configuration.

Authoring walkthrough with runnable examples:
**[docs/COMMANDS_HOWTO.md](../../../docs/COMMANDS_HOWTO.md)**; the field
reference is CONFIGURATION.md § commands/.

## What it adds

- **Commands**: one per markdown file, named by the file stem
  (`commands/deploy.md` → `/deploy`).
- Compatible with opencode's existing command files (zero-copy).

## Discovery

Two separate hooks, because they run at two different times against two
different registries:

1. **Global** (`<config>/commands/`) — registered once, at server startup,
   into the process-wide command registry. Trusted: this is the operator's
   own config.
2. **Project-local** (`<project_root>/commands/` and
   `<project_root>/.opencode/commands/`, opencode's own layout, walking
   ancestors up to the git root) — registered lazily, the first time a
   session rooted at that project asks for its command catalog
   (`ProjectCommandSource`), into a registry scoped to that root alone.
   Untrusted: working-tree-supplied content, treated accordingly by the
   command surface (disclose-and-confirm before every run — see
   COMMANDS_HOWTO.md §2).

A project command may freely shadow a global one of the same name for
sessions rooted there — the same rule a project's own `agents/<name>.md`
already follows for presets. Nearest directory wins a stem collision within
the project walk.

## Trust and collisions

- Global config-dir commands are trusted; project-local commands are
  untrusted.
- An untrusted file whose command name is already taken (by a nearer project
  dir) is SKIPPED, not registered — the earlier registration wins.
- A trusted (global) name collision is a fatal startup error, same as any
  other command-registry collision; a project-registry collision cannot be
  fatal (there is no startup moment to fail at), so it is simply skipped with
  a log line.

## Configuration

None.
