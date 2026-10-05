# Example skills (`skills/<name>/SKILL.md`)

A skill is a directory containing one `SKILL.md`: YAML frontmatter with a
required single-line `description:` (an optional `name:` must match the
directory), and a markdown body the `skill` tool returns verbatim on
invocation. The directory name is the skill's identity.

Skills are **templates to copy** — this directory is not a discovery path.
Each example dir (`repo-etiquette/`, `eda-checklist/`, `ctf-triage/`) is the
unit to copy — the whole directory, not just the file:

```sh
mkdir -p ~/.config/minimal/skills
cp -R eda-checklist ~/.config/minimal/skills/     # global

# or project-local — one library for every harness that reads it:
mkdir -p .agents/skills
cp -R eda-checklist .agents/skills/               # also read: skills/, .claude/skills/
```

Project-local skills shadow global ones of the same name; the nearest
directory wins. Discovery reads only frontmatter; the body is lazy-loaded on
invocation, so a large skill library is cheap to have.

Skills come from the `skills` plugin and only surface where a preset opts in —
`plugin_permissions: {skills: ["*"]}` in its frontmatter (or per-name entries;
`permissions.json` `plugins.skills` sets the operator default). Access modes,
per skill name:

| Mode | Behavior |
|---|---|
| `allow` | listed; the tool loads it outright |
| `ask` | listed; each load prompts (grant key `skill:<name>`) |
| `deny` | hidden; the tool refuses |
| `preload` | body ships in the system prompt at session start; not tool-callable |

Example: `plugin_permissions: {skills: {repo-etiquette: preload, "*": allow}}` — the
conventions ride in every prompt, the rest load on demand. Subagents clamp
elementwise; a child can never widen into its parent's preloads.

| Directory | Demonstrates |
|---|---|
| `repo-etiquette/` | A project-conventions skill — the `preload` case: short, always-on, per-project |
| `eda-checklist/` | A checklist skill — on-demand procedure, pairs with `agents/data-analyst.md` |
| `ctf-triage/` | A methodology skill — pairs with `agents/ctf.md` |

Reference: `plugins/server/skills/README.md`.
