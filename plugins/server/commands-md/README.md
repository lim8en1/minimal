# commands-md

Markdown-defined `/-commands`: drop a `.md` file in a commands directory and it
becomes a `/command`. One plugin, no configuration.

## What it adds

- **Commands**: one per markdown file, named by the file stem
  (`commands/deploy.md` → `/deploy`). Registered once, at server startup.
- Compatible with opencode's existing command files (zero-copy).

## Discovery

Directories walked, nearest-first; the nearest directory wins a stem collision
(project shadows global):

1. The server process's cwd and its ancestors up to the git root:
   - `<dir>/commands/`
   - `<dir>/.opencode/commands/` (opencode's own layout)
2. The global config dir: `<config>/commands/`

## Trust and collisions

- Global config-dir commands are the operator's own: trusted.
- Project-local commands are working-tree-supplied content: untrusted
  (treated accordingly by the command surface).
- An untrusted file whose command name is already taken is SKIPPED (the earlier
  registration wins), so a cloned repo's `commands/help.md` can never block
  the server from starting.

## Limitation

The discovery anchor is the SERVER process's cwd, not any session's project
root — commands register once, before any session exists, and the server
serves many project roots. A session whose project root differs sees only the
global (trusted) commands from this plugin.

## Configuration

None.
