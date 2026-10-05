# Bundled plugins

Everything under `plugins/server/` is loaded by the server's plugin loader
using exactly the seams a third-party plugin uses — bundled plugins are not
structurally privileged, just pre-installed. Each plugin directory has its own
README with what it adds and how to configure it.

Entries are configured in `plugins.json` (see
`src/server/plugins/plugin_discovery.cpp` for the exact shape):

```json
{
  "plugins": [
    {
      "file": "tools-fs.so",
      "provenance": "bundled",
      "enabled": true,
      "config": { "..." : "the plugin's own schema, documented in its README" }
    }
  ]
}
```

The `config` object is passed to the plugin verbatim — core imposes no schema;
each plugin owns its own, and every one documented here treats missing or
partial config as normal (defaults apply). `session-titler` and
`typesafe-advisor` ship disabled; everything else is load-bearing for the
default harness.

## Tools

| Plugin | Adds | README |
| --- | --- | --- |
| tools-fs | `read`, `write`, `edit` | [README](server/tools-fs/README.md) |
| tools-search | `grep` (RE2), `glob` | [README](server/tools-search/README.md) |
| tools-bash | `bash` (sync + background), `/restart-shell`, `/tasks`, shell-state service, before-command hook point | [README](server/tools-bash/README.md) |
| tools-python | `python_exec`, `python_interrupt`, `/python` (persistent per-session interpreter) | [README](server/tools-python/README.md) |
| tools-web | `web_fetch`, `web_search` | [README](server/tools-web/README.md) |
| tool-ask | `ask_user` (structured multiple-choice questions to the human, answered through the `ask-view` client plugin's modal) | [README](server/tool-ask/README.md) |
| ask-view | Client half of tool-ask: the `ask` question modal (prompt renderer) and the `ask` transcript row (display renderer) | [README](client/ask-view/README.md) |

## Providers

Registered under the kind string a providers.json entry's `backend` field
names. Configured in providers.json, not plugins.json
(`bundled/providers.example.json`).

| Plugin | Backend kind | README |
| --- | --- | --- |
| provider-openai | `openai` (any OpenAI-compatible endpoint, incl. llama.cpp / vLLM; model discovery) | [README](server/provider-openai/README.md) |
| provider-anthropic | `anthropic` (Messages API, prompt caching) | [README](server/provider-anthropic/README.md) |

## Commands

| Plugin | Adds | README |
| --- | --- | --- |
| commands-core | `/help`, `/compact`, `/clear`, `/agent`, `/model`, `/sessions`, `/connect` | [README](server/commands-core/README.md) |
| commands-md | One `/command` per markdown file (global + project commands dirs, opencode-compatible) | [README](server/commands-md/README.md) |

## Observability & services

| Plugin | Adds | README |
| --- | --- | --- |
| checklist | `checklist` tool + `checklist.endpoint` (per-session task list; get/list + `changes` push stream) | [README](server/checklist/README.md) |
| checklist-view | `{todo}` status segment + `checklist.panel` view (focused tasklist / per-tab summary, `ctrl+k`) | [README](client/checklist-view/README.md) |
| cost-tracker | `/cost`, startup auto-refresh + `--plugin-call cost-tracker refresh_rates` (pricing snapshots), `cost-tracker.rpc` (token usage and cost) | [README](server/cost-tracker/README.md) |
| session-titler | LLM-generated session titles after turn 1. **Ships disabled** — enable in plugins.json | [README](server/session-titler/README.md) |
| signals-message | `message_session` tool (cross-session messaging) | [README](server/signals-message/README.md) |
| skills | `skill` tool + per-skill permission model (`plugin_permissions.skills`), per-preset listing and preloads | [README](server/skills/README.md) |
| compaction-structured | the `structured` compaction strategy (`record_summary` tool call → typed summary sections) | [README](server/compaction-structured/README.md) |
| opencode-compat | opencode-shaped `providers.json` entries and opencode's `auth.json`, read through the providers.json extension seam | [README](server/opencode-compat/README.md) |
| typesafe-advisor | Jev-based permission advisor (auto-allow/auto-deny with abstain-to-human). **Ships disabled** — enable in plugins.json + permissions.json `advisor` | [README](server/typesafe-advisor/README.md) |
