# tools-fs

File tools: `read`, `write`, `edit`. Bundled and enabled by default — losing them
leaves a useless harness, not a degraded one. Uses exactly the seams a
third-party plugin uses; the tools are not structurally privileged.

## What it adds

- **Tools**: `read`, `write`, `edit`.
- Path safety is centralized: the tools declare path-confinement support and
  ToolExecutor classifies out-of-workdir access, so the rule cannot drift per tool.
- `read` windows are clamped to a hard ceiling (disclosed in the result footer);
  `limit: 0` means the ceiling, not "the whole file".
- `write` and `edit create` create missing parent directories (the confined path
  is already inside the project root, so this cannot escape it).
- `edit view` takes `view_range: [start, end]`; `end: -1` means "to the end of
  the file" (still under the read ceiling).
- In a file whose every line ends in CRLF, `insert` writes CRLF, and an LF
  `old_str` that only matches once translated to CRLF is replaced as such.
  LF and mixed files are edited byte-exact.
- A file that grows past `edit.max_editable_file_bytes` while being read is
  refused whole, never edited as a truncated prefix.

## Configuration

The plugin's own `config` object in its `plugins.json` entry, one sub-object per
tool. Every field is optional; missing values keep the defaults, and an absent
or partial config object is normal rather than an error.

```json
{
  "file": "tools-fs.so",
  "enabled": true,
  "config": {
    "read": {
      "default_limit_lines": 2000,
      "max_line_length": 2000,
      "max_lines": 2000
    },
    "edit": {
      "max_editable_file_bytes": 5242880
    }
  }
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `read.default_limit_lines` | `2000` | Window size the model sees when it supplies no `limit` (capped at `read.max_lines`). |
| `read.max_line_length` | `2000` | Per-line truncation length. |
| `read.max_lines` | `2000` | The hard ceiling on any read window, whatever the model asked for. Bounds memory proportionally to the configured limit, not the file size. |
| `edit.max_editable_file_bytes` | `5242880` (5 MiB) | Worst-case memory bound for `write`/`edit`, which must hold the whole file to rewrite it. Past this the model is told to use a different approach. |

Non-positive values fall back to the defaults rather than silently producing
empty windows.
