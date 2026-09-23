# tool-ask

The `ask_user` tool: the model asks the human 1-4 structured, multiple-choice
questions and waits for their answers.

## What it adds

- **Tool**: `ask_user` — 1-4 questions, each with 1-5 labeled options (the
  user may also type a custom answer instead of picking one). Use when a
  decision is genuinely the user's to make — ambiguous instructions,
  competing approaches, a destructive choice — not for facts the model can
  look up or choices it can reasonably make itself. Runs serialized on the
  session worker, like `bash`: one modal slot per session tab, so two
  concurrent asks would overwrite each other's prompt client-side.
- The tool IS the prompt: its only effect is the question it raises, so it is
  `ReadOnly` and never sits behind its own permission gate (the same posture
  the skill tool takes for a self-describing tool). An operator who wants to
  gate it anyway can still pin it to Ask via `per_tool_overrides` — its
  `Disclose()` shows the first question's text so that prompt is informative.

## Degrade

On a host without an interactive user attached (`ctx.ask_user` unset), the
tool answers immediately: "ask_user is not available in this session ...
decide without asking" — never a hang waiting for a human that cannot answer.
A user who dismisses the questions without answering gets a result telling
the model not to re-ask the same thing, but to proceed with a sensible
default or explain what it needs instead.

## Configuration

None.
