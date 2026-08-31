# minimal — Fourth-pass review and remediation plan

Companion to `ARCHITECTURE.md` (design, single source of truth), `docs/DEVELOPMENT_PLAN.md`
(sequencing and the record of review passes §8, §11, §12, §13), and `docs/EXTENSIBILITY.md`
(plugin surface). This document records the **fourth deliberate review pass** — everything
except track E (the TUI) — and the plan to fix what it found. Where this document and
`ARCHITECTURE.md` disagree, that document wins until the fix lands and updates it in the
same change.

**Method.** All three documents were read in full; the source tree (~32k LOC across
`src/` and `plugins/`) was reviewed in six parallel deep-dives (concurrency/session,
agent loop + compaction, LLM provider/transport, tools + bundled tool plugins, session
storage, plugin host/hooks/MCP); the six highest-severity findings were then verified
directly against the source before being recorded here. The prior passes were diffed
against — everything below is what they missed, not a restatement.

**The pattern, stated first because it predicts where the fifth pass will find things:**
almost every defect below is an **un-swept sibling of a fix the prior passes already
landed**. The decode-outside-lock fix reached `LoadMessages` but not `GetMessagesPage`;
the "locked DB is not a missing session" fix reached `SessionExists` but not `GetSession`
or `LastMessageSeq`; the keyset-seek fix reached the messages query but not
`ListSessions`; the streaming null guards did not reach the non-streaming parser; the
exception-containment discipline reached every plugin seam except the background
watcher. The second recurring class is §12's own through-line, still dominant: **a
comment describing a mechanism that isn't there** — six more instances are recorded
below. Fix-by-point-fix leaves siblings; this plan's workstreams sweep each class once.

---

## 1. Findings

Severity is the review's; every ship-blocker was verified in source directly.

### 1.1 Ship-blockers

1. **Subagents permanently stop working after N successful spawns.**
   `subagent_manager.cpp:87-98` — the `TicketGuard` destructor is the *only* place
   `starting_children_` is decremented, and it returns early when `released=true`;
   `:251` sets `ticket.released = true` on every successful spawn. Each success
   permanently inflates the parent's count; after `max_concurrent_subagents` (default 4)
   spawns — even sequential, all long finished — every future spawn refuses. The existing
   test never spawns *after* a successful one. Two adjacent flaws in the same 30 lines:
   `ChildCount` is read **outside `limit_mu_`** (`:76`, the check-then-act split the
   header comment at `:70-75` claims is impossible), and `ChildCount` counts
   finished-but-still-attached children — `session_registry.cpp:264-275` tests
   `!terminated()`, not `subagent_finished()`, the exact bug its own comment says was
   fixed. The limit is broken in both directions.
2. **Cancel/MaxTokens with completed tool blocks orphans a `tool_use`; the session then
   400s on every later request.** `agent_loop.cpp:607-616` — the assistant message is
   appended and persisted *before* the stop-reason checks; the `break` at `:609`
   (Cancelled) or `:614` (MaxTokens/ContentFilter/Refusal/StopSequence) skips tool
   execution *and* result synthesis. A cancel landing after the model finished streaming
   tool calls — the most natural cancel moment — leaves history ending in an unanswered
   `tool_use`, a hard 400 on both wire formats. The codebase knows the failure
   intimately (`ToolExecutor::Prepare`'s comment describes it;
   `synthesize_missing_tool_results` exists) but the repair runs **only on resume**
   (`session_registry.cpp:206`). The live loop has none. Adjacent provider subpath: a
   *cancelled* stream whose tool-call `arguments` were cut mid-JSON throws `ParseError`
   instead of reporting `Cancelled` (`openai_provider.cpp:149-151`,
   `openai_stream_reassembler.cpp:138-143`) — defeating the §11.4 exit-code fix for that
   subpath.
3. **An abandoned advisor thread use-after-frees the `Session`.** `session.cpp:72-73`
   binds the advisor cancel predicate as `[this] { return IsCancelled(); }` — a raw
   `Session*`. `permission_advisor.cpp:93-107` detaches the advise thread on budget
   expiry, and its "self-contained" comment is false: the shared_ptr keeps the
   `AdvisorCancelSource` alive, not the Session it points at. A slow (not even wedged)
   advisor that honours its polling contract dereferences a destroyed Session on every
   poll after teardown. Plus one leaked thread + full `PermissionRequest` copy per
   over-budget call.
4. **A hook shell action can SIGPIPE-kill the entire server.** `hook_shell.cpp:191-203`
   — on `POLLERR|POLLHUP` it still calls `write()` on the payload pipe. A hook child
   that exits without reading stdin (the *normal* case for `logger`/`notify-send`)
   closes the read end; SIGPIPE's default disposition kills the process. The only
   `SIGPIPE→SIG_IGN` in the tree is the MCP shim's (`mcp_stdio_shim.cpp:32`), installed
   **lazily** — server survival depends on whether an unrelated stdio MCP server
   happened to initialize. Payloads larger than the 64 KiB pipe buffer make death
   deterministic, and `pre_tool_use` pipes the **full model-authored `tool_input`** into
   the hook (`tool_executor.cpp:210-215`). The dev plan's "the bash tool's proven shape"
   claim is wrong: tools-bash never writes a payload to a child that may not read it.
5. **Glob brace expansion is exponential — model-authored OOM on the session worker.**
   `glob_matcher.cpp:119-149` — nested `{a,b}` alternation is translated by recursion
   and each level duplicates both alternatives into the output string; ~40 nesting
   levels (a ~320-byte pattern sent via the `glob` tool, which passes
   `kBraceAlternation`) produces a 2⁴⁰-byte string before RE2 ever sees it. The RE2
   rationale ("make the hang unreachable, not bounded") is defeated in the *translator*,
   which the walk timeout does not cover; recursion depth is also unbounded. Same
   class, same plugin family: the command scanner recurses per named child and
   tree-sitter-bash builds left-deep trees for `&&` chains
   (`command_scanner.cpp:103-110` — a long command string overflows the worker stack,
   uncatchable by the Run-guard); `tree_walker.cpp:48` recurses per directory with no
   depth cap.
6. **The resume path can crash on a transient SQLite error.**
   `session_database.cpp:611` — `GetSession` returns `nullopt` for `SQLITE_BUSY` /
   `SQLITE_IOERR` exactly like a missing row (the bug class §12 fixed for
   `SessionExists` but never swept here), and `session_service.cpp:293-294` then does
   `*fresh` on the unchecked optional — UB in the RPC handler. Same class:
   `LastMessageSeq` (`:833`) treats a failed step as "empty transcript"; consumed by
   `RunRedo`, where `pre_active_last = -1` makes `LoadAfter(-1)` load the *entire*
   transcript — duplicated history.

### 1.2 RAM/CPU (against the low-footprint goal)

7. **The outbound queue is bounded by count, not bytes — ~540 MB per stalled client.**
   `event_queue.hpp:74,136` — 65,536 events is the only bound. Bash streams 8 KiB
   chunks as events (`bash_tool.cpp:98-104`), each a fresh heap copy → ≈540 MB per
   session whose client stalls, unbounded across sessions. `ToolExecutionEvent.input`
   carries the full tool input (model-controlled size) **twice per call**
   (`agent_loop.cpp:698,713-717` reuse one struct for started and finished); grep emits
   whole lines up to `grep_max_file_bytes` = 1 MiB. The queue exists precisely for the
   stalled-client state, and the byte amplification is maximal exactly there.
8. **Model-authored reads can allocate the whole file.** `file_access.cpp:60-63` —
   `limit == 0` means unlimited; `fs_tools.cpp:86` passes the model's raw value with no
   ceiling. `{"limit": 0}` on a 10 GB log = OOM. `ARCHITECTURE.md`'s "memory
   proportional to `limit`, not file size" is true but hollow when `limit` is
   model-controlled. The edit path caps at 5 MiB — the bound was considered, never
   applied to read. `view_range` arithmetic overflows (`fs_tools.cpp:238-240`, signed
   UB landing in "unlimited read").
9. **`grep_max_results` is a dead knob.** Documented as "the absolute ceiling on
   collected results" (`search_settings.hpp:22-23`), parsed, tested — never referenced
   by `grep_tool.cpp`. Model `head_limit` is taken raw (`grep_tool.cpp:257-259`); the
   `glob` tool enforces its equivalent ceiling, grep doesn't.
10. **The one-mutex database stalls are worse than documented.**
    - `GetMessagesPage` decodes JSON **under the global mutex**
      (`session_database.cpp:713-763`) — the "decode outside the lock" fix was applied
      to `LoadMessages` (`:807-809`) but not to the hottest read path (every scrollback
      page, every `HistoryWindow` read).
    - `AppendMessage` does the JSON encode under the mutex (`:425-438`); the default
      `wal_autocheckpoint` is neither tuned nor disabled, so a committing append can
      synchronously absorb a checkpoint+fsync under `mu_` — tens of ms typical, seconds
      on slow storage. The header's "sub-millisecond append" claim
      (`session_database.hpp:85-86`) does not hold on the tool-result path.
    - `DeleteSession` cascades the entire transcript in one transaction under the
      mutex (`:590-598`).
    - `ListSessions` is an index **SCAN**, not a seek (verified with `EXPLAIN QUERY
      PLAN`): the `?3 IS NULL OR …` prefix at `:660-663` is exactly the shape §11.8
      fixed for messages — O(n²) to page the picker. The EXPLAIN invariant test
      (`session_store_test.cpp:505`) guards the messages query only.
    - `LoadAll` double-materializes (raw rows copied under the lock, then decoded;
      `row.json` copied into `parse` rather than moved — peak ≈ 2× transcript).
11. **Streaming transport contradicts the 512k–1M-token goal.**
    - The idle-timeout callback ignores upload progress (`http_client.cpp:135-146` —
      `ultotal`/`ulnow` commented out). A multi-MB request body on an uplink slower
      than ~150 KB/s is killed at 60 s as "idle", classified retryable, and fails
      identically until retries exhaust. No test covers the upload phase.
    - No total cap on stream accumulation: a server dripping one small frame per <60 s
      grows `text`/`arguments_json` forever (`openai_stream_reassembler.cpp:38,60`;
      `request_timeout=0` for streams) and wedges the worker. The error path likewise
      reads a drip-fed non-2xx body forever (`openai_provider.cpp:92-95`).
    - The provider deep-copies the whole `ChatRequest` per call **and per retry
      attempt** (`openai_provider.cpp:65`) — tools vector with all schema DOMs — solely
      to flip the `stream` flag; the full-history body serialization also happens once
      per *attempt* inside the provider, so the "built once per tool round" claim holds
      only above the provider seam.
12. **Tool input is copied ~6-10× per call** (lifecycle event ×2 with JSON→Struct
    conversion, prepared invocation, hook payload, permission request) — for a 1 MB
    Write payload, megabytes of transient churn on the worker, twice into the
    count-bounded queue.
13. **Search tools**: RE2 pattern + glob filter compiled per call; `.gitignore`
    re-parsed and re-compiled on every walk; a directory's full listing materialized
    and sorted with cancel/timeout checked only afterwards (`tree_walker.cpp:54-83`) —
    a million-entry dir is ~100 MB of paths, uninterruptible. Model catalog copies each
    entry's full JSON DOM (`openai_model_catalog.cpp:75-77`).
14. **The third queue is silently unbounded.** Work queue (32) and event queue (65,536)
    are documented bounded; `InjectionQueue` (`injection_queue.hpp:38-45`) is not —
    producers are non-blocking subagent completions and background task results
    (up to 1 MB each) landing on an idle parent.
15. **Smaller**: `BuildChatRequest` reported to deep-copy the history per provider call
    (`agent_loop.cpp:137`) — *verify-first*: `chat.hpp` declares `messages` a borrowing
    span, so this is likely a misread; verify before touching. Header accumulation in
    `HttpClient` uncapped (`http_client.cpp:100-108`). Background watcher polls 10 Hz
    per detached thread (bounded 4/session — accepted). `TryPush` returns `true` on a
    closed queue, so drop-logging never fires after `Terminate` — the logging contract
    the surrounding comments promise is false on the closed path.

### 1.3 Correctness

**Provider/streaming** (the §12.12 fixes were not swept to siblings):

16. Non-streaming parse: `"usage": null` or `"finish_reason": null` throws `type_error`
    → non-retryable `ParseError` (`openai_response.cpp:51-52`; the streaming path
    guards both — the asymmetry proves it was known). Same class: `"id": null` in a
    chunk (`openai_stream_reassembler.cpp:22-23`); null token fields in `ParseUsage`;
    negative/`>INT_MAX` usage values propagate unchecked.
17. SSE `event: error` frames are swallowed as no-ops (the `SseFrame::event` field is
    parsed and discarded) → an error becomes an empty turn presented as success. A
    stream that closes cleanly without `finish_reason`/`[DONE]` finalizes as
    `StopReason::Unknown` and is **accepted as a complete turn**
    (`openai_provider.cpp:162-170`).
18. `Retry-After` is parsed, carried, tested — and ignored by the only retry consumer
    (`agent_loop.cpp:79-96` never consults it); parsed only for 429 and only the
    seconds form (`retry-after-ms` ignored); `std::stoll` overflow/negative unchecked
    (`openai_error_mapping.cpp:29-38`). HTTP 408 maps to non-retryable `RequestError`
    (`:46`). `finish_reason: "refusal"` maps to `Unknown` — `StopReason::Refusal` is
    dead code (`openai_response.cpp:5-11`).
19. A single `text_block_index_` reorders content arriving after tool calls —
    mis-assembled content on backends that narrate between calls
    (`openai_stream_reassembler.cpp:34-39`). SSE spec gaps: no BOM strip (first data
    line dropped), bare CR not a line terminator, `[DONE]` with trailing whitespace
    dies as `ParseError` (`sse_parser.cpp:54-62`). `*choice.tool_name` is an unchecked
    optional dereference (`openai_request.cpp:66`).

**Session/concurrency:**

20. The `user_prompt_submit` veto path returns early and skips `NotifyTurnCompleted`
    (`session.cpp:867-887`) — violating the every-exit-path obligation the file's own
    comments declare (`:932-941`); per-turn observer state leaks.
21. `attached_` refcount race: a client attaching between another's `TryAttach` and
    `fetch_add` (`session_service.cpp:456-472`) can be counted as zero → spurious
    `on_last_client_detached` → shutdown requested with a client provably attached.
    `OpenSession`'s catch only handles `std::exception` (`:491-502`) — anything else
    leaks the refcount permanently.
22. Concurrent resume of the same session: both callers run the full construction
    (transcript load, torn-tail repair append → duplicate rows, worker start,
    `OnSessionCreated`) before the map-insert backstop refuses the loser
    (`session_registry.cpp:249`).
23. **No wire event for session termination** — a session terminated under an attached
    stream (a child killed by a cancelled blocking spawn) goes silent; the handler
    no-ops forever and the client cannot distinguish "killed" from "finished quietly".
24. `Cancel` cannot unwedge a worker parked in `EventQueue::Push` on a full queue
    (stalled-but-connected client) — the one state where the user needs cancel is the
    one it cannot reach; only stream close helps. `Server::Wait`'s shutdown has no
    deadline (`server.cpp:82-91`) — a half-open stream blocks process exit
    indefinitely.
25. Manual `/compact` never re-seeds occupancy (`Compact()` doesn't touch `tokens_`) →
    a spurious second summarization on the next turn. Occupancy estimates exclude the
    system prompt, tools array, and project context (which backend ground truth
    includes) → under-trigger on resume, contradicting the `SeedOccupancy` comment's
    own claim (`agent_loop.hpp:190-193`).
26. `CancelSource::Mint` vs `CancelAll` race — a token minted after the cancel snapshot
    is never cancelled (`cancel_token.hpp:67-89`).

**MCP client:**

27. `~McpConnection` runs **under `handle.mu`** (`mcp_client_manager.cpp:150-154`) and
    blocks 250 ms–5 s (reap grace) — stalling every concurrent tool call's alive-check
    behind a destructor, contradicting the manager's stated invariant
    (`mcp_client_manager.hpp:22-25`). The caller's `shared_ptr` copy puts the same
    destructor on the session worker for an already-timed-out call.
28. The comment "the next tool call respawns the handle" (`mcp_client_manager.cpp:88`)
    is **false** — `HandleInvoker::CallTool` only errors "not connected"
    (`:41-57`); a server dying mid-session leaves its tools erroring until the next
    session start. No failure backoff: a dead server pays full
    spawn+initialize+discovery per session start and per subagent spawn at spawn rate
    (`session.cpp:242`, `subagent_manager.cpp:138`). Per-session
    `disabled_mcp_servers_` is never cleared — `/agent` (`session.cpp:380`) does not
    re-check what session-start marked dead.
29. `mcp-session-id` echoed from server response headers reaches libcurl unsanitized —
    CRLF injection (or silent header drop, libcurl-version-dependent)
    (`mcp_connection.cpp:301-312,355,389`). `${VAR:-default}` fails closed with a
    misleading error naming `API_KEY:-x` as the variable; bare `$VAR` passes through
    literally (`env_interpolation.cpp:27-34`).
30. `ReapChild`/`WatchChildDetached` SIGKILL escalation skips the pid-ownership
    recheck `AbortChild` carefully makes (`mcp_stdio_shim.cpp:128-152`) — the header's
    pid-reuse guarantee is violated by its own reapers.

**Plugins/hooks:**

31. A plugin whose registration callback throws mid-way leaves its earlier
    registrations live forever, attributed to a "failed" plugin
    (`plugin_loader.cpp:216-223`) — a half-initialized surface serving the model with
    no warning.
32. Plugin-name hardening gaps: `nullptr` from `minimal_plugin_name()` crashes the
    loader (unchecked `std::string(const char*)`, `:153-155`); duplicate
    `minimal_plugin_name()` across `.so`s is not a collision and silently shares one
    storage namespace and one `data_dir` (discovery keys by file basename,
    `plugin_discovery.cpp:161-162`); a name containing `/` or absolute escapes the
    data root. `PluginStorage`'s "one plugin cannot read another's rows —
    structurally, not by convention" is false — the namespace is the caller-supplied
    first argument (`plugin_storage.hpp:15-19`). Teardown order contradicts the
    "process lifetime" promise (`main.cpp:155` vs `:201`). `Keys()` is unbounded and
    holds the one global storage mutex (`plugin_storage.cpp:210-233`).
33. Hook executor residue: `pipe()`-failure cleanup reads uninitialized fds
    (`hook_shell.cpp:86-94`); hook children get no `RLIMIT_FSIZE` (bash sets it); the
    stdin payload (`tool_input` verbatim) is unbounded.
34. The background-task watcher runs on a detached thread with **no exception
    containment** (`background_registry.cpp:49-52`) — one throw = `std::terminate` =
    the whole server. Every other plugin seam is wrapped; the one that must outlive
    the invocation is not.
35. The `chdir`-failure diagnostic is written before stderr is redirected to the pipe
    (`bash_process.cpp:183-197`) — the message goes to the server's stderr, the model
    sees bare `exit 127`, precisely in the deleted-session-cwd case where guidance
    matters.
36. `message_codec.cpp:101` — `tool_result.content` present-but-not-array is
    **silently dropped** (empty content, no error), violating the strict-decode
    contract its own header declares while the sibling wrong-type check three lines up
    throws.
37. `read_window` silently truncates lines at NUL bytes beyond the 8 KiB binary sniff
    (`file_access.cpp:100`) — no marker, corrupted model view.

### 1.4 Corner cuts / dead config

38. **`compaction_keep_recent_tokens` is parsed, seeded, wired into `LoopConfig` — and
    never read.** `settings_config.cpp:71-72`, `main.cpp:404`, `agent_loop.hpp:42`,
    zero consumers; the cut is always half-by-message-count (`agent_loop.cpp:356-357`)
    — the exact §12 theme ("a settings block nothing reads") that the third pass
    nonetheless missed.
39. Persisted `token_count` (backend-reported output tokens) is written on every
    append, read on every path — and never consumed; resume re-derives rough estimates
    instead of using exact counts.
40. No guard that `kSchemaVersion` equals the last migration's version
    (`session_database.cpp:160-222`) — a migration added without the bump re-applies
    (or is skipped) forever.
41. Bash sessions are seeded with the server's **entire** `environ` — including
    provider API keys (`bash_session_state.cpp:15-24,35`). A model-authored `env`
    dumps them into the persisted transcript. The seeding decision is documented; the
    credential-exposure consequence is not.
42. Strict decode as a distribution risk: one unknown block type in a stored
    transcript makes every older binary unable to scroll *or* resume that session
    (`message_codec.cpp:118`) — for a public GitHub release with mixed versions, an
    upgrade hazard.

### 1.5 Architectural

43. **One SQLite connection + one global mutex for every session.** Every fix in this
    area has been point-wise while the structure guarantees the stall class keeps
    regenerating (items 10, 15). And it is process-local: `session_db_path` is
    configurable, nothing prevents a second server on the same DB, and the resume
    guard is in-process only — two live Sessions on one transcript would interleave
    appends with divergent in-memory working sets, which is worse than a crash.
44. **"Stream close terminates the session" with no terminal event.** For a
    distributed team product this is fragile in combination: a network blip kills
    in-flight work (deliberate, documented), *and* a session terminated server-side
    under a live stream goes silent (item 23). The inverse of the documented contract
    has no protocol answer.
45. **The bounded-queues story is asymmetric** (items 7, 14, and the model-input
    family 5, 8, 9, 12): count bounds without byte bounds do not bound RAM when
    payload size is model-controlled — and payload size is model-controlled almost
    everywhere. RE2's lesson ("unreachable rather than bounded") was applied to one
    input class; the same class exists as OOM/stack-overflow in sibling code.
46. **Signal/fd hygiene is an emergent property** (items 4, 33, and the §7 CLOEXEC
    gap): SIGPIPE disposition set lazily by one subsystem and implicitly relied on by
    another; pipe-failure cleanup reading uninitialized fds; every bash child
    inheriting gRPC/SQLite/log fds. Process-lifetime guarantees rest on subsystems
    that do not know about each other.

---

## 2. Decisions taken (recorded here because the fixes depend on them)

| Decision | Choice | Rationale |
|---|---|---|
| Scope | Everything — all findings including architectural items, as one planned effort | The un-swept-sibling pattern argues for sweeping each class once, not fixing points |
| Model-visible behavior changes (read ceiling, grep ceiling, bash env) | **Ceilings + config knobs**, safer defaults, user-tunable | These are RAM issues, not permission issues; the gate does not bound allocation |
| `message_codec` forward compat | **Degrade with marker**: unknown block type → readable placeholder `TextBlock` + warning; wrong-typed fields keep throwing | A public release with mixed versions must not brick scrollback/resume; silent dropping stays refused |
| `SessionEnded` wire event | **Add now** — additive proto oneof case, emitted on every terminate path | The headless client needs it immediately; the silent-handler gap is not deferrable |
| Multi-process DB stance | **Lock file at open** — exclusive `fcntl` lock on `<db>.lock`, holder pid recorded, second instance refuses | Makes the single-process assumption true structurally instead of by convention; `fcntl` locks die with the process, so no staleness |
| `compaction_keep_recent_tokens` | **Implement it** — token-based cut selection from per-message estimates | Matches the documented intent; subsumes the `token_count`-never-consumed gap (item 39) |

Proposed knob defaults (adjustable before landing, not after): EventQueue byte cap
64 MiB (constant); `read_max_lines` 2000; `grep_max_results` 1000 (already parsed);
glob translated-regex cap 64 KiB / depth 16; walker depth cap 1024; per-directory
entry cap 10 000; `tools.max_input_bytes` 10 MiB; `InjectionQueue` cap 64 messages;
hook payload cap 64 KiB; `tools.mcp.restart_cooldown_ms` 30 000;
`bash.inherit_env` `allowlist|all` (default `allowlist`).

---

## 3. Remediation plan

**Conventions, every item (DEVELOPMENT_PLAN §6 discipline):** each fix lands as its
own commit-sized change; the invariant test that proves it load-bearing ships with it
(delete the fix, watch the test fail); zero warnings; `ARCHITECTURE.md`, the affected
headers' comments, and `DEVELOPMENT_PLAN.md` §7 are corrected **in the same change** —
comment-vs-code drift is a defect to fix, not documentation to file later.
**Verification bar per workstream:** full suite + the `asan` preset + the `tsan`
preset, clean.

**Verify-first items** (resolve before or during the owning workstream): whether
`BuildChatRequest` copies the history (item 15 — `chat.hpp` says span; fix only if
real); whether the linked gRPC runtime already ignores SIGPIPE (moot once W1 makes it
explicit); whether vcpkg's curl port builds the threaded resolver (if not, document
the DNS-hang caveat — neither `CONNECTTIMEOUT` nor the progress callback can interrupt
a synchronous `getaddrinfo`).

### W1 — Signal & process hygiene *(first: makes everything else testable safely)*

**Status: landed.** Full suite, `asan` and `tsan` presets green (1034 tests each). Items
1.1–1.6 all landed as specified, plus the findings the work surfaced along the way: the
`poll`→`write` race means the POLLHUP gate alone cannot prevent the SIGPIPE death — the
process-wide disposition is the load-bearing half (the new hook test dies by signal
without it); `CancelToken::Cancel` used the invalid `memory_order_acq_rel` on a store;
three pre-existing TSan races in test code (titler's `FakeSupportService`, the
background-test harnesses' event vectors, `rewind_test`'s counting observer and
`HeadlessFixture`'s databases torn down under an in-flight observer task) — fixed via
locked accessors, drained observers, and the watcher's terminal state now stored LAST
("state is terminal" ⇒ the watcher's writes are quiescent), plus two latent
zero-warnings violations (`session_observer.hpp` unused parameter, `agent_loop` ctor
init order).

| # | Fix | Files |
|---|---|---|
| 1.1 | `std::signal(SIGPIPE, SIG_IGN)` at the top of both `main()`s; the shim's call stays (idempotent) | `src/server/main.cpp`, `src/client/app.cpp` |
| 1.2 | `POLLOUT` only gates the hook stdin write (no write on `POLLERR/POLLHUP`); `pipe()` failure cleanup: fds initialized to −1, sequential creation, both arrays closed — mirror `bash_process.cpp:114-123` | `src/server/hooks/hook_shell.cpp:86-94,191` |
| 1.3 | `dup2`s move **before** `chdir` in the bash child so the chdir-failure diagnostic reaches the pipe | `plugins/server/tools-bash/bash_process.cpp:183-197` |
| 1.4 | Hook child gets `RLIMIT_FSIZE` (mirror bash) | `hook_shell.cpp:121-126` |
| 1.5 | Background watcher body wrapped in `try/catch(...)` → log + emit `failed` task event | `background_registry.cpp:49-52` |
| 1.6 | CLOEXEC sweep at serve-stage start: walk open fds (`/proc/self/fd` / `/dev/fd`), set `FD_CLOEXEC` on all > 2 — retires the §7 fd-hygiene gap structurally | new `src/server/config/fd_hygiene.{hpp,cpp}` + `main.cpp` |

*Tests:* hook child exiting without reading + >64 KiB payload → server survives,
fail-open logged; fd-exhaustion seam test of the cleanup branch; deleted-cwd test —
the model sees the message, not bare `exit 127`; `ls /dev/fd` from a live bash session
shows only intended fds.

### W2 — Subagent admission trio (ship-blocker 1)

1. Give `TicketGuard` an explicit `Release()` (decrement under `limit_mu_`, then set
   `released`); the success path calls it at `subagent_manager.cpp:251` instead of
   `ticket.released = true` — the "microscopically conservative" window the comment
   describes becomes real. *Test:* 10 sequential spawns at limit 4 all succeed (fails
   today).
2. Move the `ChildCount` read inside the `limit_mu_` critical section; verify the
   `limit_mu_ → registry` lock order has no reverse path and document it at both ends.
   *Invariant test:* limit 1, 5 concurrent sibling spawns → ≤ 1 child created.
3. `ChildCount` counts `!subagent_finished()`, matching its own comment
   (`session_registry.cpp:264-275`). *Test:* a finished-but-attached child tab does
   not consume a slot.

### W3 — Orphaned tool_use + reassembly hardening (ship-blocker 2)

1. **Live repair:** on every break path at `agent_loop.cpp:609/614` with non-empty
   `tool_uses`, synthesize error `ToolResultBlock`s and append+persist as one results
   message. Extract `synthesize_missing_tool_results` from `session_registry.cpp:206`
   into shared domain code; the resume path switches to it. *Invariant test:* cancel
   after tool blocks streamed → the **next turn succeeds over the wire** (the 400
   regression), for both the Cancelled and MaxTokens-with-tool_calls variants.
2. **Cancel vs truncated args:** when the sink reports cancellation, the provider
   drops tool calls with unparsable `arguments` instead of parse-throwing
   (`openai_provider.cpp:149-151`). *Test:* cancel mid-arguments →
   `StopReason::Cancelled`, not `ParseError`.
3. **Defense-in-depth** (the one unguarded seam left on the §12.2 path):
   `ToolPool::WorkerMain` wraps `task()` (`try/catch(...)` → error result);
   `execute_one`'s lifecycle-sink calls wrapped (log-and-continue — a throw there must
   not unwind `RunTurnLoop` past pool tasks holding references to its locals);
   `TruncateAfter`'s empty-history branch sets `head_is_summary_ = false`;
   `Compact()` with `first_persisted_seq == nullopt` sets `head_is_summary_ = true`.

### W4 — Advisor lifetime (ship-blocker 3)

Bind the cancel predicate through `SessionChannels` — the `shared_ptr` block built for
exactly this class of handle: channels gains a shared cancel state (terminated flag /
cancel generation) that `Terminate` sets; `session.cpp:72-73` captures the channels
`shared_ptr`, not raw `this`. The abandoned thread then reads a live flag after the
Session dies, returning "cancelled". Budget and thread-abandonment behavior unchanged
and documented. *Invariant test (ASan bar):* slow-but-polling advisor + session
destroyed underneath → predicate returns true, no UAF.

### W5 — Provider/transport robustness + CPU

| # | Fix | Files |
|---|---|---|
| 5.1 | Idle callback tracks **upload** progress (`ulnow` change resets `last_progress`) | `http_client.cpp:135-146` |
| 5.2 | Total stream-accumulation cap (64 MiB) enforced in the reassembler (`text +=`, `arguments +=`) → classified `LlmError`; the error-body drip aborts at its 64 KiB cap instead of reading forever | `openai_stream_reassembler.cpp:38,60`, `openai_provider.cpp:92-95` |
| 5.3 | Non-streaming null guards (`finish_reason`, `usage`, chunk `id`, null token fields), mirroring the streaming fixes; negative/absurd usage clamped | `openai_response.cpp:51-52`, `openai_stream_reassembler.cpp:22-23` |
| 5.4 | SSE: strip a leading BOM; lone CR is a line terminator; `[DONE]` compared on trimmed data; **`event: error` frames surfaced** as a classified error (the parser's event field reaches the consumer) | `sse_parser.cpp:54-62`, `FeedFrame` signature |
| 5.5 | Clean close without `finish_reason`/`[DONE]` → classified error, never `StopReason::Unknown` accepted as a complete turn | `openai_provider.cpp:162-170` |
| 5.6 | Classification: 408 retryable; `Retry-After` parsed for any status + `retry-after-ms` + overflow/negative sanitized; **`SleepForBackoff` honors `retry_after`** (wait ≥ it, cancel-polling preserved); `refusal` → `StopReason::Refusal`; `*choice.tool_name` checked | `openai_error_mapping.cpp:29-46`, `agent_loop.cpp:79-96,575`, `openai_request.cpp:66` |
| 5.7 | Kill the per-attempt `ChatRequest` deep copy (thread `stream` through `BuildRequestBody`); serialize the body **once per tool round**, reuse across retry attempts | `openai_provider.cpp:39,65` |
| 5.8 | `HttpClient` header accumulation capped (64 KiB → abort); slist-append null guard | `http_client.cpp:100-108,225` |
| 5.9 | Text arriving after tool calls opens a **new** text block (no reordering of narrating backends) | `openai_stream_reassembler.cpp:34-39` |

*Tests:* loopback HTTP server variants (slow upload, drip feed, clean close mid
stream, `event: error`); canned bodies for the null guards; retry-wait floor honoring
`Retry-After` (the equal-jitter floor pattern from §8); body-reuse asserted via a
counting echo server. Resolve the item-15 span question here.

### W6 — RAM ceilings: the model-input allocation audit (decision: ceilings + knobs)

| # | Fix | Knob (default) |
|---|---|---|
| 6.1 | **EventQueue byte bound**: `ByteSizeLong()` computed once at `Push`, running byte total, cap alongside the count cap, same blocking/refusal semantics; `TryPush` returns false on a closed queue (drop-logging fires); lifecycle events carry `input` on **started only** | 64 MiB (constant) |
| 6.2 | **read/edit ceiling**: model `limit` (incl. `0`) clamped; clamping disclosed in the result footer; `view_range` computed in int64 with overflow rejected; `offset` narrowing validated | `read_max_lines` (2000) |
| 6.3 | **grep ceiling**: the dead `grep_max_results` wired (clamp + footer) | 1000 (already parsed) |
| 6.4 | **Recursion/translation caps**: `GlobToRegex` output cap + depth cap → pattern refused as an error result; command scanner → iterative walk (explicit stack); tree walker → iterative DFS + depth cap | 64 KiB regex / depth 16; walker depth 1024 |
| 6.5 | Directory listing: cancel/timeout checks inside the listing loop; oversize directory discloses the skip | 10 000 entries/dir |
| 6.6 | Global tool-input ceiling: a `tool_use` whose serialized input exceeds the cap is refused with an error result before execution | `tools.max_input_bytes` (10 MiB) |
| 6.7 | `InjectionQueue` bounded (refusal logged; producers already degrade) | 64 messages |
| 6.8 | Bash env seed: filtered allowlist (PATH, HOME, USER, SHELL, TERM, TERM_PROGRAM, LANG, LC_*, TMPDIR) + knob restoring old behavior; the credential-exposure consequence documented | `bash.inherit_env`: `allowlist`\|`all` |
| 6.9 | Model-catalog metadata move-constructed instead of copied | — |

*Invariant tests:* remove the read clamp → the size assertion fails; the nested-brace
glob is refused in bounded time (deadline harness); a 50k-term `&&` chain scans
without stack death; `limit: 0` on a large file returns the clamped window + footer.

### W7 — Persistence layer

1. **Error-discipline sweep** (the class §12 fixed once and never swept):
   `GetSession` and `LastMessageSeq` distinguish step failure from missing (throw or
   error out-param, the `SessionExists` pattern); the `*fresh` deref guarded
   (`session_service.cpp:293-294`); `RunRedo` failure → `ErrorEvent`, never a
   duplicated transcript. *Tests:* corrupted/locked DB → `INTERNAL`, no UB, no
   duplicate history.
2. **Lock scope:** the `MessageToJson` encode hoisted out of the mutex in
   `AppendMessage`; `GetMessagesPage` steps raw rows under the lock and decodes after
   (the `LoadMessages` fix, swept); `LoadAll` moves `row.json` into `parse`.
3. **Checkpoint stalls off the write path:** `wal_autocheckpoint=0` + passive
   checkpoint on the existing reaper thread every N appends; a benchmark documents
   the before/after (the "verify by execution" bar).
4. **`ListSessions` seek:** cursor predicate via `COALESCE` (no `? IS NULL OR`
   prefix); the SQL extracted to a shared constant; **EXPLAIN tests assert SEARCH
   against the production statement** for both paged queries (also closing the
   lookalike-test gap, `session_store_test.cpp:522-527`). *Test:* O(page) paging over
   a 1000-session database.
5. **Lock file (decision):** exclusive `fcntl` lock on `<session_db_path>.lock` at
   open, held for the process lifetime, holder pid written into the file; a second
   instance refuses startup with a named error. Same treatment for `plugins.db`.
   *Test:* second open fails with the pid in the message.
6. **`DeleteSession`:** chunked message deletes in sub-transactions + an orphan sweep
   at startup; the partial-delete-on-crash tradeoff documented.
7. **Codec stance (decision):** unknown block type → placeholder
   `TextBlock{[unknown block type "x" — written by a newer version]}` + warning;
   wrong-typed fields — including the silently-dropped `tool_result.content`
   (`message_codec.cpp:101`) — keep throwing. *Tests:* a newer-binary transcript
   scrolls and resumes on an older binary; wrong-typed content still throws.
8. Migration guard: `kSchemaVersion == migrations.back().version` asserted when the
   list is built + test.
9. `read_window` NUL truncation: a visible marker, consistent with the overlong-line
   footer (`file_access.cpp:100`).

### W8 — Session/registry concurrency + protocol

1. **`attached_` race:** increment before `TryAttach`, decrement + release on attach
   failure; `catch(...)` alongside `catch(std::exception)` in `OpenSession`.
2. **Concurrent resume:** a registry reservation — the `session_id` is claimed under
   the registry mutex before construction (guard releases on failure); duplicate
   construction, double torn-tail repair, and phantom observer events become
   impossible. *Test:* two concurrent resumes → one `ALREADY_EXISTS` before any side
   effect.
3. **`SessionEnded` event (decision):** additive proto oneof case; `Terminate` emits
   it before `Close` so the writer drains it; the headless client handles it (stop
   reading, exit cleanly); the silent no-op handler loop ends. *E2E:* a cancelled
   blocking subagent → the client receives `SessionEnded` for the child.
4. **Cancel-unwedge:** `EventQueue::AbortWaitingProducers()` called from `Cancel` and
   `Terminate`; a parked `Push` wakes and returns false; the sinks' existing
   bool-return contract turns that into a loop abort. *Test:* full queue + stalled
   writer + `CancelTurn` → the worker joins promptly.
5. `Server::Wait` shutdown gets a deadline (30 s) then hard shutdown, logged.
6. The veto path emits `NotifyTurnCompleted` — restructured so the bottom block cannot
   be returned past (scope guard), the same every-exit-path shape as the catch blocks.
7. `Compact()` re-seeds occupancy; the estimate is extended to include the system
   prompt and tools array (`EstimateContext` helper used at every seed site).
8. `CancelSource::CancelAll` holds the collection lock through cancellation — `Mint`
   cannot land in the snapshot window.

### W9 — MCP client

1. `StartHandle`: the old connection is moved out under `handle.mu` and destroyed
   **after** releasing; a timed-out caller hands its last `shared_ptr` to a
   reaper-cleaned graveyard — no 5 s destructor on a session worker and no stall
   behind `handle.mu`.
2. **Make the code match the comment** (or the comment match the code — here, the
   code): implement respawn-on-next-tool-call (dead server → `EnsureAlive` under the
   existing per-server gate) + a per-server failure cooldown
   (`tools.mcp.restart_cooldown_ms`, default 30 000) so subagent spawn loops cannot
   storm a down server; `/agent`'s `RefreshMcpServers` clears the session's
   `disabled_mcp_servers_`.
3. Header hygiene: CR/LF rejected/stripped in header values at `mcps.json` parse
   **and** on the `mcp-session-id` echo.
4. `ReapChild`/`WatchChildDetached`: pid-ownership re-checked immediately before
   SIGKILL.
5. `${VAR:-default}` supported; bare `$VAR` warned at parse (literal passthrough
   documented as such).
6. Comment truths fixed while there: the 2 s TTL (`mcp_client_manager.hpp:53`), the
   `record.status = Loaded` ordering (`plugin_loader.cpp:323`).

### W10 — Plugin host & hooks

1. Plugin-name hardening: null-checked `name`/`abi_tag`/`version` (load failure with a
   diagnostic); duplicate `minimal_plugin_name()` across `.so`s is fatal; names
   validated as plain names (no `/`, not absolute — protects `data_dir`).
2. Partial registration: `PluginRecord` gains a distinct `PartiallyLoaded` status —
   seams registered before the throw stay live but are **reported** as
   active-but-partial (Warning, named) at startup and in the sanity check;
   `PluginStorage`'s "structurally" claim corrected to convention; teardown order
   fixed (`plugin_storage` outlives the registries); `Keys()` gets a LIMIT + cap.
3. Hook payload cap: `tool_input` truncated in the envelope at 64 KiB with a
   `truncated` flag (W1.2's EPIPE fix already stops the crash; this bounds the burn).

### W11 — Compaction: `compaction_keep_recent_tokens` (decision: implement)

Per-message token estimates maintained in `AgentLoop` alongside `history_` — exact
persisted `output_tokens` for assistant rows (finally consuming `token_count`),
bytes/4 otherwise; rebuilt in one pass on load. Cut selection walks from the tail
accumulating estimates until the configured budget is reached, **then** boundary-snaps
— the snap rule is unchanged and its tests must pass untouched. Sanity warning when
the budget ≥ the context window (degenerates to "never cut"). *Invariant tests:* a
giant recent tool result plus tiny old messages → the cut lands before the giant
result (fails against count-based); the snap invariant unchanged; the resume path
rebuilds estimates (subsumes item 39).

### W12 — Final sweep

Doc truth pass over every corrected comment; `DEVELOPMENT_PLAN.md` §7 updated (fd
hygiene retires, SIGPIPE retires, `mcps.json` args-with-spaces stays); the
model-input-allocation-ceiling rule recorded in `ARCHITECTURE.md` as a §12-style
through-line entry so the fifth pass does not rediscover the class; the
curl-resolver question answered or documented.

---

## 4. Sequencing

**W1 → W2 → W3 → W4** (the verified ship-blockers, each independently shippable),
then **W5–W7** (the largest workstreams; they may interleave), then **W8–W11**, and
**W12** last. The only cross-workstream dependencies: W11 consumes W7's persisted
`token_count` reading, and W6.1 (byte accounting) precedes W8.4 (abort-waiters build
on it). Every workstream is merge-safe on its own.

## 5. Deliberately not fixed (recorded as decisions, not oversights)

- **Background-watcher 10 Hz idle poll** — bounded by `max_background_tasks` (4) per
  session; converting to event-driven liveness notification is complexity the bound
  does not justify.
- **Detached-thread leaks that are bounded by design** — the wedged advisor and
  wedged MCP worker each cost one thread plus their captured state; un-joinable
  threads are the documented containment shape.
- **Arguments-before-id delta visibility** — tool-call argument fragments arriving
  before `id`/`name` are hidden from the streaming sink (the final response is
  complete); making the UI show them is not worth the wire noise.
- **`CurlGlobalInit` static-destruction risk** — a transfer in flight during process
  teardown is already outside every lifetime guarantee the tree makes.
