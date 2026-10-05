---
# A reference preset: every structural frontmatter field, annotated. Copy it,
# then delete what you do not need -- most presets use a handful of these.
# Field-by-field semantics: docs/CONFIGURATION.md, "agents/ and prompts/".
# Names like `github`, `deploy` or `house-style` are placeholders: the startup
# check warns about any server or skill your config does not define.

# --- identity and listing -----------------------------------------------------
name: reference                  # display name; the preset KEY is the file stem
description: Every preset field in one place, annotated.
mode: primary                    # all | primary | subagent (subagent = hidden from the picker)
color: "#7aa2f7"                 # agent picker colour
# hidden: true                   # not listed, still spawnable
# disable: true                  # refuses to resolve, not listed

# --- model ----------------------------------------------------------------------
# A <provider>/<model> ref from providers.json; omit to inherit
# settings.default_model. A reasoning variant is addressed with @<name>.
# model: anthropic/claude-opus-4-5@high
compaction_strategy: structured  # summarize | a plugin's strategy (compaction-structured)
# compression_prompt: terse-compression   # prompts/<name>.md for summaries

# --- tools: what the model can see --------------------------------------------
# Fail-closed: no allowed_tools and no plugins means NO tools. Entries are
# permission keys (never aliases); ["*"] is every registered tool.
allowed_tools: [ask_user, checklist, skill, spawn_subagent]
# Every tool these plugins register (by plugins.json name), now and later.
plugins: [tools-fs, tools-search, tools-bash]
# Dynamic tool members a plugin offers: here, mcps.json servers by name.
tool_sources:
  mcp: [github]
# Who this preset may spawn (preset keys = file stems). Absent = any;
# [] withholds spawn_subagent entirely.
allowed_subagents: [explore, general]
steps: 40                        # tool-round ceiling when run AS a subagent

# --- permissions: what happens when a visible tool is called -----------------
permission_mode:
  read_only: allow
  file_write: ask
  execute: ask
  out_of_workdir: deny
  network: ask
  overrides:                     # per tool: {tool: mode}
    checklist: allow
  patterns:                      # per tool: {tool: {glob: mode}}
    bash:
      "git status*": allow
      "git log*": allow
      "rm *": deny
    read:
      "**/.env": deny
# Each plugin permission model's slice, verbatim. A preset entry for a name
# replaces permissions.json's plugins.<plugin> entry for that name.
plugin_permissions:
  skills:                        # list form = all allow; map form per skill
    "*": allow
    house-style: preload         # body shipped in the system prompt
    deploy: ask                  # loading prompts (grant key skill:deploy)
    secrets-rotation: deny       # hidden and refused
  mcp:
    servers: {github: ask}       # per server: allow | ask | deny
  tools-web:
    hosts: {"docs.python.org": allow, "*.example.com": deny}

# --- per-plugin tool configuration -------------------------------------------
tool_config:
  python:
    packages: [numpy]            # ensured into the per-project venv

# --- anything else is a provider request parameter --------------------------
# Unrecognized keys go to the provider verbatim (overriding the model's
# providers.json parameters). Request fields the harness owns are ignored.
temperature: 0.3
---

You are a general assistant configured from the reference preset. This body is
the system prompt, sent as-is; project context (AGENTS.md / CLAUDE.md) is
appended after it.

Prefer reading before changing anything, ask before destructive steps, and say
plainly when a tool you need is not available to you.
