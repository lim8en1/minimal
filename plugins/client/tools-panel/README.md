# tools-panel

Every tab's tool calls in one panel, with their live output.

## What it adds

A panel view, `tools.panel` (default key ctrl+t): per tool call, its status,
name, session, summary, start time and elapsed time or duration, then the last
lines of its output. It spans every open tab -- the chat shows one session,
this shows what is running anywhere -- and follows the newest activity. Lossy
by design: the output is the client's capped live buffer; each session's
transcript is the record.

## Configuration

`client-plugins.json`, this plugin's `config`:

- `output_lines` (integer, at least 1, default 100): trailing output lines
  shown per tool.
