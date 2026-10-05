# compaction-structured

The `structured` compaction strategy: when the context is compacted, the
summarizer answers through a strict `record_summary` tool call whose fields
become typed sections of the summary — `current_task`, `done`, `next_steps`,
`topics`, `important` (`{name, why, how?}`), `lessons`, `future_tasks`.

## What it adds

`minimal_plugin_register_compaction_strategies` → `"structured"`: core's cut
selection, this summarizer, and `record_summary` as the strategy's request
tool. Core keeps that tool on every main request (the summarizer request is
the main request plus one instruction, so it stays a prompt-cache hit) and
answers a stray call outside a compaction with a no-op error.

A model that answers in text instead, a custom compression prompt (preset
`compression_prompt:` or `/compact <name>`), or a missing instruction file all
degrade to core's free-text summary.

## Configuration

- `settings.json` `memory.compaction_strategy: "structured"` (the seeded
  default) or a preset's `compaction_strategy: structured`.
- `prompts/compression-structured.md` — the instruction and field guidance,
  read once at load.

## Disabled

A setting or preset naming `structured` warns at startup and compacts with
core's `summarize`.
