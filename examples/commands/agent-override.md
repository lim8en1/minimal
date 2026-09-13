---
description: Run one turn on a borrowed agent preset and model without switching the session
agent: plan
model: openai/gpt-4o-mini
---
For this turn only, work as the plan preset (investigate and propose, change
nothing) and report what you find about the working tree:

!`git status --short`

Then propose, as a plan, the smallest set of changes the status above calls
for. Do not make them.
