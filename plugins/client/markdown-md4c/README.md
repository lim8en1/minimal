# markdown-md4c

The markdown engine: how assistant text, compaction summaries and markdown
tool results render.

## What it adds

The exclusive markdown-engine slot, filled with an md4c adapter: GFM
(tables, task lists, strikethrough), fenced code colored by the host's
highlighter (`ClientPluginRegistrationContext::highlighter`, over every
registered grammar), and streaming text reparsed on a debounce -- partial
input cannot soundly parse, so a growing message is reparsed at most once
per window (longer windows for long messages) and `Finalize()` always
reparses authoritatively. A reparse the debounce held back asks the host
for its frame (`HostUi::RequestRedrawAfter`).

With this plugin disabled, markdown reads as plain text.

## Configuration

None.
