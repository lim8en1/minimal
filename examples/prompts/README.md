# Example system prompt fragments (`prompts/*.md`)

Prompt fragments are plain markdown (any frontmatter is stripped and the
body used) that the server stitches into system prompts. These files are
**templates to copy** — this directory is not a discovery path.

There are two hook points:

| File | Hook point | How it gets used |
|---|---|---|
| `terse-compression.md` | **Compaction** (per-preset) | When context hits the compaction trigger, the model is asked to summarize the history with this prompt. The default is `prompts/compression.md`; a preset may point elsewhere with `compression_prompt: <name>` in its frontmatter. |
| `terse-subagent.md` | **Subagent prefix** (global) | Prependied to every spawned subagent's system prompt. The default is `prompts/subagent.md`; there is exactly one, shared by all presets. |

To use one, copy it into the config dir's `prompts/` and restart the server:

```sh
cp terse-compression.md ~/.config/minimal/prompts/   # available as "terse-compression"
```

`terse-compression.md` only takes effect once some preset names it:

```markdown
---
name: brief
compression_prompt: terse-compression
---
You are a coding agent. ...
```

`terse-subagent.md` is a **replacement**, not an addition: copy it over
`prompts/subagent.md` in the config dir (seeding never overwrites your copy):

```sh
cp terse-subagent.md ~/.config/minimal/prompts/subagent.md
```

Missing `prompts/compression.md` only warns — compaction silently does
nothing until a prompt exists — so a bad copy here is a behavior change, not
an error. Reference: `docs/CONFIGURATION.md`, "agents/ and prompts/".
