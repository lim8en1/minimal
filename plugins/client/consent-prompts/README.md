# consent-prompts

The permission and confirmation modals: how core asks the user.

## What it adds

Prompt renderers for core's two required prompt kinds (`docs/EXTENSIBILITY.md`
§4.2): `permission` (a tool call the permission model resolved to Ask, with
its session-grant offers) and `confirmation` (the server's confirmations,
e.g. `/clear`, and the TUI's own quit confirmation). Both draw their chrome
with the widget kit.

Core sends the payload, keeps the prompt layer topmost and fails closed; this
plugin decides only the look. A replacement must keep the kinds' wording
obligations: grant labels verbatim (they are blanket, for the whole binary),
an incomplete disclosure says what it does not cover, and nothing is answered
on the user's behalf.

To replace it, disable this plugin in `client-plugins.json` and enable one
that renders both kinds. With neither kind rendered the TUI refuses to start.

## Configuration

None.
