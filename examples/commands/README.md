# Example `/`-commands (for `commands-md`)

Markdown-defined `/`-commands: each `commands/*.md` file becomes a
`/command`. These files are **templates to copy** — this directory is not a
discovery path, so nothing here is loaded as-is.

To use one, copy it into a discovery directory and restart the server:

```sh
mkdir -p commands
cp greet.md commands/                    # project-local: untrusted — every
                                         # invocation confirms before running
# or, trusted, no per-run prompt:
cp greet.md ~/.config/minimal/commands/  # ($XDG_CONFIG_HOME/minimal if set)
```

Files (walked through step by step in
[docs/COMMANDS_HOWTO.md](../../docs/COMMANDS_HOWTO.md)):

| File | Demonstrates |
|---|---|
| `greet.md` | Minimal command: template + required/optional args, no sources |
| `branch-review.md` | `run:` argv sources, positional substitution, optional argv-entry omission, `output_cap: fail` |
| `release-notes.md` | opencode syntax: `$1`, `!`cmd`` shell injections (desugared at load) |
| `prompt-from-script.md` | No-body mode: a script whose stdout IS the whole prompt, `output_cap: confirm` |

Notes:

- Frontmatter must be the first line of the file, and **everything after it
  is prompt body** — there is no comment syntax inside a command file. (The
  `run:`/`sh:` examples assume a git repository; `release-notes.md` assumes
  tags exist, since `git describe --tags` fails without one.)
- `prompt-from-script.md` invokes `./scripts/compose-review-prompt.sh` from
  the server's working directory — point it at any script that prints a
  prompt to stdout.
- Copying into the project's `.opencode/commands/` works identically
  (opencode's own layout, loaded as-is).
