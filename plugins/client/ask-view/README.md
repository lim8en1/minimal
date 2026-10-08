# ask-view

The client half of the `tool-ask` plugin pair. Core has no ask UI; this plugin
supplies both pieces, each keyed by kind `ask` (never by tool name, so an
aliased `ask_user` keeps them):

- **Prompt renderer `ask`**: the modal that the server's `ask_user` tool
  blocks on (`ToolContext::prompt_user`, kind `ask`).
- **Display renderer `ask`**: the transcript row for the call. It shows every
  question with its answer, or the reason there is none (dismissed, timed
  out, cancelled, no client could answer). It starts expanded because it is
  the user's only record of what they answered once the modal closes.
  chat-view's `fold_tools: "collapsed"` (its `client-plugins.d/chat-view.json`) still collapses it.

## Keys

| Key | While editing |
| --- | --- |
| Enter | Single-pick list: pick the highlighted row and move to the next question. On "Type your own answer" it moves into the text line instead. Checkbox list or text line: next question. After the last question it opens the review list, except a lone single-pick question, which submits at once |
| 1-9 | Pick (single) or toggle (checkbox) that row |
| Space | Pick or toggle the highlighted row |
| Tab | Skip to the next question, leaving this one as it is |
| Esc | In the text line: back to its list. Elsewhere: dismiss. Once anything is answered, a second Esc is needed |

In the review list, Enter submits and Esc returns to editing. Ctrl+C always
reaches the host's turn-cancel.

## Degrade

With this plugin disabled, the host answers every `ask` prompt `UNAVAILABLE`.
The model is told nobody can answer and decides by itself. The transcript row
falls back to the plain text row.
