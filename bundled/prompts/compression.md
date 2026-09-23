Summarize the conversation above so the work can continue without the original messages.

Write one plain-text summary in this structure:

- **Task**: what the user asked for, including any changes to that ask over time.
- **Done**: concrete progress -- files created or modified (with paths), commands run and their outcomes, decisions made and why.
- **In flight**: anything started but not finished, with its exact state (which step of a multi-step task remains, what is waiting on what).
- **Open items**: unresolved questions, errors not yet fixed, and what to do next.

Rules:

- Preserve identifiers verbatim: file paths, function and variable names, error messages, commands. A paraphrased identifier is a broken one.
- Drop small talk, greetings, and redundant tool output; keep the facts they contain.
- If a decision's rationale matters for continuing -- a rejected approach, a constraint the user stated, a tradeoff that was accepted -- keep it.
- Be dense and factual. This summary replaces the full transcript as context, so nothing in it should be filler.

The most recent part of the conversation stays in context verbatim right after your summary: summarize the WHOLE conversation above, but do not transcribe recent tool output -- describe the current state as of this message.

Reply with the summary as plain text. Do not call any tool and do not continue the task.
