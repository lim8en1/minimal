# How-to: custom dynamic `/`-commands

Drop a markdown file into a commands directory and type its name in the
session — `/do-something X`. The file can run scripts, collect their output,
and compose the prompt that is submitted as the turn. No configuration, no
code: one plugin (`commands-md`) loads every `commands/*.md` at server
startup.

This is a walkthrough. The terse field-by-field reference lives in
[CONFIGURATION.md](CONFIGURATION.md) § commands/. Ready-to-copy files are in
[`examples/commands/`](../examples/commands/).

---

## 1. The one rule that shapes everything

**The body never executes anything.** It is an inert render template, filled
in a single pass from already-evaluated values. All execution lives in the
frontmatter's `sources`, which run in declaration order, flat and
independent. Values inserted into the prompt are never re-scanned — nothing a
generator printed is ever interpreted as more template, more shell, or more
config.

That one ruling buys the rest: a command file can only do what its author
wrote in `sources`, and the operator can read that block and know exactly
what will run.

## 2. Where files go

Nearest directory wins a stem collision; project shadows global:

| Directory | Trust |
|---|---|
| `<project_root>/commands/`, `<project_root>/.opencode/commands/` — the session's own project root and ancestors up to the git root | untrusted (project-local) |
| `<config>/commands/` (your own config dir) | trusted |

The two rows register at different times, because there is no session (and so
no project root) until one exists:

- **Global** (`<config>/commands/`) registers once, at **server startup** —
  restart the server after adding or editing a file there.
- **Project-local** registers lazily, the **first time a session rooted at
  that project** asks for its command catalog, and is cached for that root —
  **rebuilt automatically** when a file there is added, edited or removed
  (the next invocation runs the new version; no restart). The TUI re-pulls
  its autocomplete list for the focused tab's project in the background
  whenever autocomplete or `/help` is open (at most every 2 s), so a new
  command shows up within a moment of typing `/`.

A project command may shadow a global **markdown** command of the same name
for sessions rooted there. It never shadows a built-in or plugin command
(`/secret`, `/clear`, `/model`, ...): the built-in keeps the name, and the
project file with that name is neither listed nor run.

Untrusted means: every invocation discloses each source's literal
program/script, plus any `agent:`/`model:` override, and asks run/cancel
before anything executes. Trusted commands run without that prompt, like
every other command in the harness.

Sources run in the **session's project root**, with a filtered environment
and stdin at `/dev/null`.

## 3. A minimal command: template + arguments

```markdown
---
description: Greet someone in the session        # autocomplete text
args: [name]                                     # required, positional order
optional: [tone]
---
Greet {name} warmly. Keep the reply to a sentence or two.
```

`args` bind to positionals in order, `optional` after them:

```
/greet Ada formally
```

Unknown-but-identifier-shaped `{tokens}` in the body (`{nane}`) do not abort
anything: unknown placeholders pass through as literal text and a one-line
warning names the token once per render. An empty composed prompt, a missing
required argument, or a failed source — those are the hard errors; nothing is
submitted on any of them.

## 4. Running scripts: `sources`

Each source runs a program at invocation time; its stdout becomes a value.

```markdown
---
description: Review the working tree's current branch
args: [focus]
optional: [count]
sources:
  branch:
    run: ["/usr/bin/git", "branch", "--show-current"]   # argv form
  changes:
    run: ["/usr/bin/git", "diff", "--stat", "HEAD~{count}"]   # argv form
    output_cap: {max: 4000, on_exceed: fail}
---
The current branch is {branch}.
Review the uncommitted work with a focus on {focus}:

{changes}
```

- **`run:` is an argv list** — each entry reaches `execve()` directly, no
  shell ever parses it. Substituted values cannot be reinterpreted
  regardless of their content.
- **`sh:` is a shell string** — run as `/bin/sh -c`. Values substituted into
  it are escaped for the quoting they land in (single-quoted when bare,
  escaped inside `"..."` or `'...'`, so opencode's `"$1"` gets exactly the
  value), and it may reference **only user-supplied arguments** — never
  another source's output. Generator output (network
  data, git log text) must not be spliced into a shell string.
- A source declaring **both** `run:` and `sh:` fails to load. A file mixing
  declared sources with the `!`cmd`` shortcut (§7) loads but is lint-warned.
- Sources run **in declaration order**, and later sources can use earlier
  ones' values in `run:` argv entries (`{branch}` in a later `run:`) — but
  not in a `sh:` string (§4's shell restriction), and not in the
  frontmatter of anything (sources are fixed at load).
- Substitution in sources is **lenient**: only known names substitute; JSON
  payloads, awk programs, and shell `${var}` braces pass through verbatim.

## 5. Using outputs in the prompt

Every value the invocation has, by the time the body renders:

| Placeholder | Filled from |
|---|---|
| `{args}` | all positionals joined with spaces |
| `{1}` … `{9}` | positional N |
| `{argname}` | a required `args:` entry |
| `{optname}` | an `optional:` entry (unfilled renders as nothing) |
| `{sourcename}` | that source's stdout (trailing newlines stripped) |
| `{output:src.field}` | declared field of a JSON source's stdout |

JSON sources declare their expected fields, which are then addressable
individually:

```yaml
sources:
  summary:
    sh: "make summary"
    output: {type: json, fields: [plan]}
```

A missing field, or stdout that is not valid JSON, is a hard error — nothing
is submitted.

## 6. Optional arguments and the command line

An unfilled optional binds as **empty** for the body (renders nothing) and
drives one addition rule: a `run:` **argv entry** that references an unfilled
optional is omitted **whole** — that is what "optional" means on a command
line. In the example above, `/review x` runs `git diff --stat`; `/review x 2`
runs `git diff --stat HEAD~2`.

A `sh:` script cannot omit pieces — an optional it references that was not
provided is an error, not a silent gap.

## 7. opencode shorthand (desugars at load)

Existing opencode command files work zero-copy:

- `$ARGUMENTS` → `{args}`; `$1` … `$9` → positionals (whole-token only).
- `` !`cmd` `` → an anonymous `sh` source, and the token becomes `{cmd1}`,
  `{cmd2}`, … in appearance order. `$1`/`$ARGUMENTS` **inside** the script
  desugar too — they are the command's own arguments.

```markdown
---
description: Draft release notes from recent commits
---
Draft release notes for $1.

Commits since the last tag:
!`git log --oneline $(git describe --tags --abbrev=0)..HEAD | head -n 40`
```

`@file` expansion is not implemented.

## 8. No-body mode: a script that writes the whole prompt

Omit the body and the **sole text source's stdout IS the prompt** — useful
when a CLI utility computes the entire thing (zero or more than one text
source with no body is a load/invocation error):

```markdown
---
description: Compose a review prompt from the repository's own state
sources:
  composed:
    sh: "./scripts/compose-review-prompt.sh"
    output_cap: {max: 20000, on_exceed: confirm}
---
```

## 9. Guardrails

- **`timeout:`** (seconds per source, default 600, `0` = infinite): at the
  deadline the process group is SIGKILLed and the run fails.
- **`output_cap:`** (token estimate, default ~50000): there is no truncation
  — half a prompt costs the same as a whole one and does half the job. On
  exceed, `on_exceed: confirm` asks submit/cancel and, if you submit, ships
  the already-capped first run's output (a re-run would repeat the
  generator's side effects); `on_exceed: fail` aborts.
- **Failures are status lines, never turns.** Nonzero exit, timeout, missing
  JSON field, declined cap → an error status line; nothing is submitted,
  nothing reaches the model, nothing enters the transcript.
- **Generators run with a filtered environment** (`PATH`, `HOME`, `LC_*`,
  …) — never the server's own, so a command file cannot silently read a
  provider API key through an env-var leak.
- **Untrusted (project-local) commands disclose and confirm** their sources
  before every run — a repo-shipped `commands/*.md` never executes anything
  the first time you type its name without asking.

## 10. Per-turn model and agent

```yaml
---
model: anthropic/claude-sonnet-4-5
agent: plan
---
```

The turn this command submits runs on the given model and/or agent preset;
the session's sticky choices are untouched. A malformed `model:` refuses to
load (shape is checked at parse time).

## 11. Trying the examples

[`examples/commands/`](../examples/commands/) holds the four files used in
this walkthrough. Copy one into a discovery directory and restart the server:

```sh
cp examples/commands/greet.md commands/        # project-local (untrusted)
# or, to make it trusted:
cp examples/commands/greet.md ~/.config/minimal/commands/
```

(`.opencode/commands/` works too — opencode's own layout, loaded as-is.)
