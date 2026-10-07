# chat-renderers

How each kind of chat row and display payload looks.

## What it adds

A renderer for every chat row kind -- your messages and the assistant's
(markdown, through the markdown engine), thinking, tool activity, attached
files, system notes, agent/model switches, compactions -- and for every
display payload kind a tool can send: diffs and files (syntax highlighted),
processes, markdown. Long rows split into chunks so they scroll smoothly.

Every one is registered as a fallback: a plugin that claims one kind (say,
its own look for tool activity) replaces just that one, with no conflict.
Disabled, a row with no renderer reads as its plain text.

## Configuration

None.
