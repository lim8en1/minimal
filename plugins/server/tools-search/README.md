# tools-search

Search tools: `grep` and `glob`. Bundled and enabled by default — searching a
tree through `bash` round-trips results through the context window with no
structure and no ignore rules; a dedicated search tool is the token-efficiency
point of the harness.

## What it adds

- **Tools**: `grep` (RE2, linear time — a pathological model-authored pattern
  cannot wedge the session worker), `glob` (full tree listing, bounded the same
  way).
- **Paths are project-root-relative** whatever the `path` argument searched:
  read/write/edit resolve against the project root, so a returned path is a
  literal tool argument rather than one the model must re-anchor. The walker
  itself keeps matching gitignore rules and glob patterns against
  walk-root-relative paths; the prefix joins at emission only.
- Path safety is centralized in ToolExecutor, exactly as for tools-fs.
- The `grep` timeout bounds the directory WALK; RE2 already guarantees the
  regex itself cannot hang. Partial results are reported on expiry.
- `.gitignore` files are honored from the project root down, also when `path`
  names a subdirectory: an ancestor's rules keep their anchoring (a root
  `/build` does not ignore `src/build`). A `path` that is itself inside an
  ignored directory is searched as asked; its own `.gitignore` still applies.
- Permission path rules apply to every file a walk reaches, not only to the
  `path` argument (core supplies `ToolContext::path_permitted`). A file the
  rules deny, or would ask about, is withheld and counted in the footer; the
  default dotenv deny reaches walked `.env` files the same way. Files too large
  for `grep.max_file_bytes` are skipped whole, never searched as a truncated
  prefix.
- `glob` walks the whole tree (within its timeout) and returns the newest
  matches with an exact total; `glob.max_results` caps the rows returned, not
  the walk.

## Configuration

The plugin's own file, `plugins.d/<name>.json` (one per plugin, beside `plugins.json`), one sub-object
per tool. Every field is optional; non-positive values fall back to defaults.

`plugins.d/tools-search.json`:

```json
{
  "grep": {
    "default_head_limit": 250,
    "max_results": 1000,
    "max_file_bytes": 1048576,
    "timeout_ms": 10000,
    "max_context_lines": 10,
    "max_line_length": 2000
  },
  "glob": {
    "default_limit": 200,
    "max_results": 2000,
    "timeout_ms": 10000
  }
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `grep.default_head_limit` | `250` | Result count the model sees when it supplies no `head_limit`. |
| `grep.max_results` | `1000` | Absolute ceiling on collected results. |
| `grep.max_file_bytes` | `1048576` (1 MiB) | Files larger than this are skipped (whole, and disclosed). |
| `grep.timeout_ms` | `10000` | Hard wall-clock bound on one invocation. |
| `grep.max_context_lines` | `10` | Context lines shown around a match. |
| `grep.max_line_length` | `2000` | Per-line truncation length. |
| `glob.default_limit` | `200` | Result count the model sees when it supplies no limit. |
| `glob.max_results` | `2000` | Ceiling on paths one call returns (a larger `head_limit` is clipped and disclosed); the walk itself and the total are not capped. |
| `glob.timeout_ms` | `10000` | Hard wall-clock bound on one invocation. |
