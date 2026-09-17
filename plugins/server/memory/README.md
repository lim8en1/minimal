# memory

The dynamic-skill / dynamic-context plugin: a durable lesson store the model
grows itself, per worktree (scope `project`) and machine-wide (scope
`global`). The unit of memory is the **lesson** -- how to drive this repo's
tests, which debugger workflow cracks its startup races, what the user means
by X -- not source text.

## Tools

Four tools, one write path:

| Tool | Purpose |
|---|---|
| `memory_search` | Semantic (embedding) or recency search; keyword prefilters match text, origin file path and topic names; `topic` filter; `query=null` browses. |
| `memory_store` | **Exactly one of `text`/`path`.** `text` = inline lesson (when `allow_inline`). `path` = markdown file ingested as a group; re-ingesting the same path REPLACES its entries (the refinement loop). |
| `memory_get` | Full entry text; for ingest chunks, position + sibling ids. |
| `memory_delete` | Single-entry delete; delete+store supersedes. |

The first memory call of each session carries a `memory_digest` (counts,
by-origin, top topics, backend, pending re-embeds). `/memory` shows the
operator the same digest plus all topics, and `/memory purge <path>` bulk-removes
an ingest group.

## Backends (`config.strategy`)

- `"auto"` (default): `onnx` if the model loads, else `http` if `api_base` is
  set, else lexical-only. The winner is reported by `/memory` and the digest.
- `"http"`: OpenAI-shape `POST {api_base}/embeddings` (llama.cpp server, vLLM,
  OpenAI...). Zero extra dependencies.
- `"onnx"`: local, in-process (ONNX Runtime, CPU EP). Needs two operator-provided
  files under the config data dir (`plugins/memory/`):

  ```
  <data_dir>/models/model.onnx        # e.g. all-MiniLM-L6-v2 / bge-small-en-v1.5 ONNX export
  <data_dir>/models/tokenizer.json    # HF fast-tokenizer file for the same model
  ```

  v1 tokenizers: **WordPiece only** (the models above). One download, no runtime
  network, nothing sent anywhere.
- `"none"`: no vectors; search is the lexical prefilter only. The store stays
  fully usable.

No embedding backend configured is a normal state, not an error: tools answer,
search degrades, `/memory` says `lexical only`.

## Enrichment (`summarizer_model`)

Off by default. Set `"summarizer_model": "<provider>/<model>"` (resolved
through `core.session_support`, like session-titler) to have a small LLM write
a one-sentence summary and pick/create a topic per stored entry -- asynchronously,
never blocking the store, silent-degrade on any failure.

Topics are a capped per-scope taxonomy (`max_topics`, default 4096): new topics
are refused at cap and the entry stays unfiled; nothing is ever trimmed.
Provenance (model / ingest / compaction origin, source path) is metadata on the
row, never a topic.

## Why the plugin ships disabled

`DefaultsDisabled` (plugin_discovery.cpp): it writes to disk on the model's
initiative and, under the `http` strategy, sends entry text to an external
endpoint. Enabling it is one plugins.json edit:

```json
{ "file": "memory.so", "enabled": true,
  "config": {
    "strategy": "auto",
    "model_path": "models/model.onnx",
    "tokenizer_path": "models/tokenizer.json",
    "api_base": "http://127.0.0.1:8080/v1",
    "api_key_env": "MEMORY_API_KEY",
    "model": "nomic-embed-text",
    "summarizer_model": "llamacpp-local/qwen3-4b-instruct",
    "allow_inline": true,
    "max_topics": 4096,
    "max_text_bytes": 8192
  }
}
```

`allow_inline: false` is the file-ingestion-only, DB-backed mode: entries enter
only via `memory_store(path=...)`; the SQLite store stays the artifact.
