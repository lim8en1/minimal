The harness is compacting this conversation's context. Call the record_summary tool exactly once with a summary of the WHOLE conversation above, up to this message. Do not call any other tool and do not continue the task.

The most recent part of the conversation stays in context verbatim right after your summary, so do not transcribe recent tool output -- but current_task and next_steps must describe the current state, as of this message.

Fields:

- current_task: what the agent is doing and why -- the task, progress so far, and what is failing or waiting on what. The keystone.
- done: concrete completed work, oldest to newest: file paths, commands with their outcomes, decisions made and why.
- next_steps: what to do next, in order. Empty only when the task is complete or the agent is waiting on the user.
- topics: one sentence per distinct topic the session may need to find its way back to.
- important: {name, why, how} -- name is a verbatim file path, tool name, MCP call or topic; how is how to use it when continuing (empty string if obvious).
- lessons: solutions to problems actually encountered in this session, and rules the user stated for it. Not generic best practices.
- future_tasks: longer-horizon work after the current task, with its why and open questions.

Identifiers -- file paths, tool names, commands -- must be verbatim; a paraphrased identifier is a broken one. Be dense and factual; use empty strings and arrays for fields with nothing to say.
