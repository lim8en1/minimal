# skills

`SKILL.md` instruction bundles the model loads on demand, or that ride in a
preset's system prompt.

## What it adds

- **`skill` tool** — `{name}` → the skill's `SKILL.md` body, prefixed with a
  `skill directory: <dir>` header so the model can read sibling files.
- **Permission model `skills`** — the gate on that tool, per skill name.
  Because it is a permission model, core's pipeline applies to it: the
  subagent ceiling, operator per-tool denies, run modes, and the `skill:<name>`
  grant ("always" covers later loads of that skill only).
- **Preset extensions** — the tool's description lists the skills the preset
  may load (sorted; the tool is withheld when it may load none), and
  `preload` bodies join the preset's system prompt. A subagent is clamped to
  its parent's skills (`preload < allow < ask < deny`, most restrictive wins).
- **Trust prompt** — a project's `skills/`, `.claude/skills/` and
  `.agents/skills/` are counted in what `/trust-directory` would enable.

## Where skills come from

`<config dir>/skills/<name>/SKILL.md`, plus — for a trusted project root —
the project's own `skills/`, `.claude/skills/` and `.agents/skills/` along
the ProjectDirs walk (nearest wins; a project skill shadows a global one).

## Configuration

Which skills a preset gets:

```yaml
# agents/<name>.md
plugin_permissions:
  skills: ["*"]                     # every loaded skill, on demand
  # or per name:
  # skills: {"*": allow, house-style: preload, deploy: ask, secret: deny}
```

`permissions.json` `plugins.skills` (same shape) is the operator default; a
preset's entry for a name replaces it. Absent everywhere = no skills.

## Disabled

No `skill` tool, no listing, no preloads; a preset's `plugin_permissions.skills`
draws a startup warning (no such permission model).
