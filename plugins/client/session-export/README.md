# session-export

`/export` saves the focused session's transcript to a file.

## What it provides

- **`/export`** opens the **export session** modal:
  - **File:** defaults to `minimal-<session id>-<YYYYmmdd-HHMMSS>.md` in the
    directory the client was started from. A relative path lands there too,
    and `~/` means your home directory. A `.txt` extension writes plain text;
    anything else writes markdown. An existing file is only replaced on a
    second Enter.
  - **Export thinking** (off by default) and **export tool calls** (on by
    default). Turning tool calls off also drops their results, and a turn that
    had nothing else in it disappears with them.
  - **Start from:** the first message, or any compaction. The compaction row
    shows its time, whether it was `auto` or `manual`, how many messages it
    folded, and the start of its summary. Starting there writes that summary,
    then every message after the ones it replaced.
- Enter exports. The outcome (the path and message count, or why it failed)
  stays in the modal until you close it with Escape.

`/export` is read-only, so it works on a subagent tab too.

## Data path

When the modal opens, the plugin's own worker reads the whole session through
`ClientApi::GetMessages`, newest page first, along with the compactions each
page carries. The file is rendered and written on that same worker, atomically
(`common::write_file`), so the UI thread never waits on the server or the disk.
If a page fails, the modal says so; reopening `/export` retries.

`/clear` is not offered as a start point. It leaves no record in the
transcript, so there is nothing for a plugin to read.
