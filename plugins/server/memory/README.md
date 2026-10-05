# memory

Durable memory the model grows itself across sessions: facts, preferences,
people, places, procedures, decisions, context. Nothing here is specific to
coding. Entries live per project (scope `project`, keyed by the worktree
root, falling back to the session root when there is no `.git`) or
machine-wide (scope `global`).

An entry has a **title**, a one-line **description** (what it is about; this
is what search, the index and tagging all key on), a **body**, optional
**tags** from a controlled vocabulary, and a **pinned** flag.

## How the model meets it

- **The Memory section.** Through the core context-contributor seam, every
  session starts with a short index in its system context:
  - how many project and global entries exist;
  - pinned entries (full bodies first, then one-liners);
  - the most-used tags with counts;
  - the most recently updated entries as `title -- description`.

  One budget (`index_budget_bytes`, default 4000, about 1k tokens) fills
  those in that order, with pinned entries capped at half. Anything trimmed
  is announced with a count and the call that fetches it ("+3 more pinned",
  "+40 more tags", "12 of 110 shown; memory_search finds the rest"), so the
  model never has to guess that more exists.

  It is rendered once per session, so the prompt cache stays stable.
  Subagents skip it unless `index_in_subagents` is set. The operator can
  turn it off with settings.json `context_contributors.disabled: ["memory"]`.
- **Tools:**

| Tool | Purpose |
|---|---|
| `memory_search` | Hybrid search over project + global entries (`scope` narrows it). `query` is natural language. Without a query it browses: pinned entries first, then the most recent, plus the full tag vocabulary with counts. `tags` is an AND filter. |
| `memory_get` | Full entries by id (up to 20). |
| `memory_store` | `title`, `description`, plus `body` inline or `path` to a markdown file. Reports a likely duplicate (`similar`). |
| `memory_update` | Edit in place: id, created_at and provenance survive. |
| `memory_delete` | By `id`, or a whole file import by `group_id`. |

The write tools are `FileWrite` by default, so they follow the operator's
write-permission policy: a stored memory outlives the session and, at global
scope, follows the user everywhere. `writes_prompt: false` makes them
prompt-free.

A preset reaches the tools by naming them in `allowed_tools`, or all five
at once with `plugins: [memory]` (docs/CONFIGURATION.md, agents). The
bundled `build`/`helper` presets take the group; `plan`/`review`/`explore`
list only `memory_search` and `memory_get`.

## Search

Lexical and semantic candidates are fused with Reciprocal Rank Fusion:

- **Lexical:** SQLite FTS5 (`unicode61`, diacritics folded) over title,
  description, tags and body. Query words are OR-ed and prefix-matched, and
  bm25 ranks with title and description weighted above the body. When that
  finds nothing (a script without word boundaries, a fragment of a word), a
  substring scan takes over.
- **Semantic:** cosine similarity over an in-memory cache of unit vectors,
  when an embedding backend is configured and reachable.

Either list alone still works. With no backend, search is lexical.

## Tags

Tags are filed by a cheap **tagger model** (`tagger_model`), never by the
main model. The main model spends no turns or tokens on them, and a store
never fails over a tag. Each tagging call sees:

- the entries' full text,
- the scope's whole vocabulary with descriptions,
- and, per entry, the tags of its most similar already-tagged entries.

It answers with existing tags, plus new ones with descriptions. A proposed
new tag becomes an existing one when:

- its normalized name matches an existing tag (case, separators, plurals), or
- its name and description embed within `tag_merge_similarity` of an
  existing tag.

At the vocabulary cap (`max_tags` per scope) only existing tags apply.
Unused tags are dropped. Without a `tagger_model`, entries stay untagged
and remain fully searchable.

The work queue is the database itself (untagged entries). So tagging
survives restarts, backfills once a tagger is configured, and re-files
entries whose text changed.

## Files as memory

`memory_store(path=...)` imports a markdown file:

- Each heading section becomes an entry, titled by its heading breadcrumb
  (`Deploy > Staging`). Headings inside code fences are ignored.
- Oversized sections split on paragraphs.
- Optional front matter may set `title` and `description`.
- Storing the same path again **replaces** its entries. That is how you
  refine a file-based memory; single imported entries are not edited or
  deleted directly.

## Operator: `/memory`

```
/memory                                  status: counts, backends, queues
/memory index                            the Memory section a session here gets
/memory tags [global]                    vocabulary with counts
/memory tags merge <from> <to> [global]
/memory tags rename <from> <to> [global]
/memory tags suggest-merges [global]     the tagger model proposes; you apply
/memory purge <path|group-id>            remove one file import
/memory reindex                          retry entries that failed to embed
```

## Embedding backends (`strategy`)

- `"auto"` (default): `sidecar` if `sidecar_model` is set, else `http` if
  `api_base` is set, else none.
- `"http"`: OpenAI-shape `POST {api_base}/embeddings` (llama.cpp server,
  vLLM, OpenAI...). `api_key` is a key reference (docs/CONFIGURATION.md
  § Secrets): `"secret:embeddings"` after `/secret set embeddings <key>`
  (read per call, so setting it later needs no restart), `"env:NAME"`, or a
  literal. A key that does not resolve fails the call, which trips the
  breaker below; entries stay pending until it resolves.
- `"sidecar"`: a long-lived python subprocess running
  [fastembed](https://github.com/qdrant/fastembed). It needs
  `sidecar_python_path` (an interpreter with fastembed installed; empty =
  `python3` on PATH) and `sidecar_model` (e.g. `BAAI/bge-small-en-v1.5`).
  The model downloads on first use into `<data_dir>/fastembed_cache/`.
- `"none"`: lexical only.

Embedding happens on one background worker: tool calls never wait for
backlog. The backend (including the sidecar spawn) is created on that
worker at first use, never during plugin registration. A failing backend
trips a circuit breaker (30 s backoff doubling to 5 min). While it is open,
search is lexical and nothing blocks. A batch that fails while the backend
is healthy is retried row by row, so one unembeddable entry is marked
failed alone (`/memory reindex` retries it). Changing the backend or model
re-embeds everything.

## Configuration

Enabled by default. Unconfigured it makes no network calls and spawns
nothing: search is lexical and entries stay untagged. Writes go through the
`file_write` permission policy. If the store cannot be opened (corrupt, or
written by a newer binary), the plugin disables itself for that run and
`/memory` says why; server startup is never affected.

Installs that already have a `plugins.json` keep their existing
`"enabled": false` entry, so there memory has to be switched on by hand.
An unknown field disables the plugin for that run, naming the field.
Full config:

```json
{ "file": "memory.so", "enabled": true,
  "config": {
    "strategy": "auto",
    "sidecar_python_path": "/usr/bin/python3",
    "sidecar_model": "BAAI/bge-small-en-v1.5",
    "api_base": "http://127.0.0.1:8080/v1",
    "api_key": "secret:embeddings",
    "model": "nomic-embed-text",
    "tagger_model": "llamacpp-local/qwen3-4b-instruct",
    "max_tags": 100,
    "allow_inline": true,
    "max_text_bytes": 8192,
    "writes_prompt": true,
    "index_budget_bytes": 4000,
    "index_in_subagents": false
  }
}
```

The remaining knobs:

- `tagger_max_tokens`, `tagger_timeout_ms`, `max_tags_per_entry`,
  `tag_merge_similarity`
- `timeout_ms`, `sidecar_startup_timeout_ms`, `sidecar_request_timeout_ms`
- `batch_size`, `default_limit`
- `index_enabled`

`allow_inline: false` is the file-only mode: entries enter only through
`memory_store(path=...)`.
