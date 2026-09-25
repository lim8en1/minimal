# tool-ask

The `ask_user` tool: the model asks the human 1-4 structured, multiple-choice
questions and waits for their answers. The client half is the `ask-view`
client plugin; core only carries the exchange (a *plugin prompt* of kind
`ask`, see ARCHITECTURE.md "Asking the user").

## What it adds

- **Tool**: `ask_user`. It takes 1-4 questions. Each has 1-5 labeled options
  (labels unique within the question), an optional `header` (at most 40
  characters), optional `multiple` (checkboxes), and optional `allow_other`
  (default true: the user may type their own answer instead). Use it when a
  decision is genuinely the user's to make (ambiguous instructions, competing
  approaches, a destructive choice), not for facts the model can look up or
  choices it can reasonably make itself. It runs serialized on the session
  worker, like `bash`, because each tab has one modal slot.
- **The tool IS the prompt.** Its only effect is the question it raises, so
  it is `ReadOnly` and never sits behind its own permission gate (the skill
  tool takes the same posture). An operator can still gate it by pinning it
  to Ask via `per_tool_overrides`; `Disclose()` shows the first question.
- **Result**: one line per question, `1. <header>: the user selected: <labels>`
  (a typed answer is JSON-quoted). The display payload (kind `ask`) records
  each question and its answer for the `ask-view` transcript row.
- **Answer validation is the tool's**, because core relays the answer
  unparsed. A label the question did not offer, two picks on a single-select
  question, free text where `allow_other` is false, or free text mixed with a
  selection all make that question "not answered". The tool never invents an
  answer.

## Outcomes

Each outcome reads differently to the model. None of them is an error, since
an error would invite a retry:

| Outcome | What the model is told |
| --- | --- |
| answered | the answers; a submission with nothing picked counts as dismissed |
| dismissed | the user declined; do not re-ask, proceed or explain what is needed |
| timed out | no answer before `sessions.unattended_prompt_timeout_seconds`; the user is probably away |
| cancelled | the question went away with the turn |
| unavailable | no one can answer here (headless client, no prompt broker, or `ask-view` disabled); decide without asking |

## Wire shape

`ask_types.hpp` defines the request payload and the answer JSON. They are the
contract with `ask-view`.

## Aliases

Agent presets (`bundled/agents/*.md`, `allowed_tools`) name the tool by its
surface name, `ask_user`. If you alias it in `plugins.json`
(`"tool-ask.ask_user": "..."`), update those presets too. The client pieces
are keyed by kind and need no change.

## Configuration

None.
