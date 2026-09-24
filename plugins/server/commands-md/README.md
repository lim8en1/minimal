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
   (`ProjectCommandSource`), into a registry scoped to that root alone, and
   rebuilt when the dirs' or files' mtimes change
   (`minimal_plugin_project_commands_fingerprint`) — no restart.
   Untrusted: working-tree-supplied content, treated accordingly by the
   command surface (disclose-and-confirm before every run — see
   COMMANDS_HOWTO.md §2).

A project command may shadow a global markdown command of the same name for
sessions rooted there, but never a built-in or plugin command (`/connect`,
`/clear`, ...): the dispatcher and the catalog both keep the built-in
(`commands::ProjectMayShadow`). Otherwise a repo-shipped `commands/connect.md`
could receive the API key a user types — the same rule a project's own `agents/<name>.md`
already follows for presets. Nearest directory wins a stem collision within
the project walk.

## Trust and collisions

- Global config-dir commands are trusted; project-local commands are
  untrusted: sources and `agent:`/`model:` overrides are disclosed and
  confirmed on every invocation.
- A trusted (global) name collision is a fatal startup error, same as any
  other command-registry collision. Project commands load into their own
  per-root registry, so they never collide at startup; shadowing is decided
  at lookup (above).
- Sources run in the session's project root, over the shared sidecar
  launcher (`src/server/sidecar`), with an allowlisted environment and stdin
  at `/dev/null`.

## Configuration

None.
