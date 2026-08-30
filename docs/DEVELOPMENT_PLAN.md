# minimal — Development Plan

Companion to `ARCHITECTURE.md`, which remains the single source of truth for *design*. This document covers only *sequencing*: what is built, what blocks what, and what "done" means for each remaining piece. Every milestone cites the `ARCHITECTURE.md` section that specifies it; where this plan and that document disagree, that document wins and this one is stale.

**Status as of the last commit (`c806728`): 284 tests passing, clean build, zero warnings.**

---

## 1. Where the project actually is

The tested surface is deep but disconnected. Nothing outside the test suite ever constructs an `AgentLoop` or a `ToolExecutor`; the server binary loads config, loads four plugins, registers five tools, prints its skills, and exits.

### Built and tested

| Subsystem | Location | Tests |
|---|---|---|
| Config: settings/providers/permissions/plugins, frontmatter, presets, skills, XDG paths | `src/server/config/` | 63 |
| Permission model: lattice, run modes, precedence chains, out-of-workdir fold, MCP policy, consent scoping | `src/server/tools/` | 40 |
| Tools: `Tool`/registry/executor, path confinement, `file_access`, `atomic_write`, `SkillTool` | `src/server/tools/` | 47 |
| Plugins: discovery, `dlopen` loader + ABI guard, collision/alias registry | `src/server/plugins/` | 16 |
| LLM: provider interface, chat/stream types, SSE parser, HTTP client, token accounting | `src/server/llm/` | 30 |
| Agent loop: tool cycle, retries, cancellation, compaction trigger + boundary snapping | `src/server/agent/` | 19 |
| `tools-fs`, `tools-bash`, `provider-openai` | `plugins/server/` | 74 |

### Not built

`src/server/rpc/`, `src/server/commands/`, `src/server/mcp/`, session storage, both clients, subagents, the production compactor, the sanity check. `commands-core` is an empty plugin shell.

### The risk this creates

Test depth is inverted relative to integration risk: every well-tested layer is exercised through fakes, and **every layer that has never executed is a layer that connects two tested ones**. Accumulated interface mismatches will surface together, on the first real session. Milestone A exists to force that moment early and cheaply, before more code is written on top of untested assumptions.

---

## 2. Dependency structure

```
                        ┌──────────────────────────────┐
                        │  A. Session + RPC + headless │  ← keystone
                        │     (§Concurrency model)     │
                        └──────────────┬───────────────┘
                                       │ unblocks
              ┌──────────────┬─────────┼──────────┬──────────────┐
              ▼              ▼         ▼          ▼              ▼
        C. /-commands   D. Subagents  E. TUI   F. Sanity     (B is not here:
        (§/-command)    (§Subagents) (§TUI)    check          it is independent)
              │              │                 (§Startup…)
              └──────┬───────┘
                     ▼
             G. MCP client (§MCP client)
```

**Independent — may start now, in any order, by anyone:**

- **B. Session storage (SQLite)** — `HistoryStore` is already an interface with an in-memory fake. A SQLite implementation is a drop-in behind it and needs nothing from A. *Caveat: the `ListSessions`/`GetMessages` RPCs that expose it do need A.*
- **F. Startup sanity check** — pure config-layer work over already-built loaders. `main.cpp` is already staged for it (`main.cpp:114`).
- **H. Production compactor** — `Compactor` is an injected `std::function`; the real one calls the provider with `prompts/compression.md`. Both sides already exist.
- **I. `SessionObserver` registry** — needed to close bash's per-session state leak (`tools-bash/plugin.cpp:64`). The ABI seam is designed; the registry is not built. Independent of A, but pointless until sessions exist, so schedule it *with* A.

**Strictly dependent on A:** C, D, E, G, and every RPC beyond `CreateSession`/`OpenSession`.

**Rationale for A first** is `ARCHITECTURE.md` § *Non-interactive client*: the headless client is "the natural E2E harness — a second reason to build it before the TUI." It converts the entire existing suite from unit-tested to actually-exercised at the lowest possible cost.

---

## 3. Milestone A — Session, RPC, and the headless client

**The keystone. Large enough to want its own plan before implementation begins.** Split into four sub-milestones that land in order; each is independently testable.

> Specified by: § *Concurrency model — `src/server/rpc/`*, § *Session lifetime*, § *Non-interactive client (`minimal -p`)*.

### A1 — `PendingRequests` and the outbound queue

The two primitives everything else in A rests on. Build and test them *first, in isolation*, because they are where the deadlock lives.

**What.** `src/server/rpc/pending_requests.{hpp,cpp}` exactly as specified in § *Concurrency model*: `AwaitResponse(request_id)` blocks the worker; `Resolve(request_id, choice)` is called from the reader. Plus `src/server/rpc/event_queue.{hpp,cpp}` — the session-owned outbound queue with a single draining writer.

**How.** `AwaitResponse` waits indefinitely by design — no timeout, no fail-closed. A client is present by construction (no detached sessions), and a human being away is not a reason to deny their work. The queue belongs to the `Session`, not the stream, so events produced before a client attaches buffer rather than drop.

**Acceptance criteria**
- `AwaitResponse` returns the value passed to `Resolve` for the matching `request_id`, and ignores non-matching ids.
- `Resolve` for an unknown id is a no-op, not a crash — a client may answer a request whose session already terminated.
- Events pushed before any writer attaches are delivered, in order, on attach.
- Terminating a session unblocks every waiter with a terminal result rather than hanging.

**Test cases** (`tests/pending_requests_test.cpp`, `tests/event_queue_test.cpp`)
- [ ] Resolve-before-await and await-before-resolve both work (the ordering race).
- [ ] Two concurrent waiters on different ids are resolved independently.
- [ ] Resolve with an unknown id: no-op, no crash.
- [ ] Termination wakes a blocked waiter; assert it returns rather than joining a hung thread.
- [ ] Queue: backlog buffered before attach, flushed in order on attach.
- [ ] Queue: single-writer invariant — concurrent producers never interleave a partial event.
- [ ] **Invariant test:** a deliberately-reintroduced "resolve only if a waiter is already registered" bug must fail the ordering-race case.

### A2 — `Session`

**What.** `src/server/agent/session.{hpp,cpp}`: owns `AgentLoop`, `ToolExecutor`, provider instance, resolved `AgentPreset`, `HistoryStore`, the outbound queue, the atomic cancel flag, and the worker thread. Plus `SessionRegistry` keyed by session id.

**How.** Three threads per session (§ *Concurrency model* table). The worker blocks freely; the reader never blocks on anything but `Read()`; one writer per attached stream is the sole `Write()` caller. Termination reuses the cancellation path — atomic flag → `bool` stream sink aborts the provider → tools observe `is_cancelled` — then joins and reaps. **No new termination mechanism.**

**Acceptance criteria**
- A session runs a full turn against `FakeLlmProvider` with a real `ToolExecutor` and real `tools-fs`/`tools-bash` tools — no fakes below the provider.
- Cancelling mid-turn aborts the provider stream and stops the worker within one tool boundary.
- Stream close after attach terminates the session; a never-attached session terminates on the ~10 s initial-attach guard (§ *Session lifetime*, "one carve-out").
- `project_root` comes from `CreateSessionRequest` and reaches `ToolContext::project_root` and bash's initial cwd.

**Test cases** (`tests/session_test.cpp`)
- [ ] Full turn: user text → tool call → tool result → assistant text, through the real executor.
- [ ] Cancel mid-turn: worker exits, no zombie thread, partial transcript retained.
- [ ] Terminate with a turn in flight: joins cleanly, no leaked thread, no use-after-free of the queue.
- [ ] Never-attached session terminates on the initial-attach guard.
- [ ] Two sessions with different `project_root`s do not see each other's bash cwd/env (integration with `BashSessionStateStore`).
- [ ] **Invariant test:** running the agent loop on the reader thread instead of the worker must deadlock the permission path — assert the worker/reader split is load-bearing rather than incidental.

### A3 — `SessionService`

**What.** `src/server/rpc/session_service.{hpp,cpp}` implementing `CreateSession` and `OpenSession` only. The other five RPCs are explicitly deferred to B and E.

**How.** Inbound dispatch is the table in § *Concurrency model* — every branch non-blocking. `OpenSession` attaches a writer to the named session's queue and flushes the backlog. Server refcounted by attached clients; exits when the last leaves (§ *Session lifetime* — no daemon).

Also lands the **wire-backed `PermissionChecker`**: emits `PermissionRequest` onto the queue, calls `PendingRequests::AwaitResponse`, maps the answer back. This is the first real implementation of that interface — today only test fakes exist — and it is where consent scoping's `PermissionDisclosure` finally reaches a human.

**Acceptance criteria**
- `CreateSession` → `OpenSession` → one `UserChatTurn` → `StreamEvent`s → terminal stop reason, over a real gRPC channel on a unix socket.
- A permission prompt round-trips: server blocks, client answers, worker proceeds. **Deny** produces an error `ToolResultBlock`, not a hang.
- `CancelTurn` interrupts an in-flight turn.
- Closing the stream terminates the session and the transcript survives.
- Server exits when the last client detaches.

**Test cases** (`tests/session_service_test.cpp` — real channel, unix socket)
- [ ] Round-trip a turn end to end; assert `ServerEvent` payload ordering.
- [ ] Permission allow and deny, both round-tripped over the wire.
- [ ] A disclosure with grants: answering with `granted_keys` silences the second prompt (proves consent scoping survives serialisation — currently only proven in-process).
- [ ] `CancelTurn` mid-stream.
- [ ] Client disconnects mid-turn: session terminates, no leaked thread.
- [ ] Two concurrent sessions on one channel do not cross-talk (`session_id` routing).
- [ ] **Invariant test:** removing the single-writer discipline must produce interleaved/corrupt writes under concurrent producers.

### A4 — Headless client (`minimal -p`)

**What.** `src/client/app.cpp` `RunHeadless` (`app.cpp:18`) and a real `SessionClient` (currently a constructor and destructor).

**How.** Connect, `CreateSession(project_root = cwd)`, send one `UserChatTurn`, stream `ServerEvent`s to stdout as text or JSONL, exit on stop reason. Defaults to `PermissionRunMode::AllowOnly` so an `Ask`-gated tool auto-denies instead of blocking on a human who was never attached (§ *Non-interactive client*).

**Acceptance criteria**
- `minimal -p "list the files here"` against a local llama server produces a real answer using real tools.
- Exit code reflects the terminal stop reason; JSONL mode is machine-parseable.
- An `Ask`-gated tool auto-denies and the run completes rather than hanging.

**Test cases**
- [ ] Scripted end-to-end against a fake provider over a real socket (CI-safe, no network).
- [ ] `AllowOnly` auto-deny path completes rather than blocking.
- [ ] Attaching to a child session is a client obligation (§ *Session lifetime*) — assert the headless client does it, since the initial-attach guard is a safety net and not a designed flow.

### Milestone A definition of done

- [ ] `minimal -p` holds a real conversation against a local llama server, using `read`, `bash`, and `skill`.
- [ ] A permission prompt is answered over the wire in both directions.
- [ ] Full suite green; clean build, zero warnings.
- [ ] `ARCHITECTURE.md` § *Concurrency model* updated if the implementation diverged — **divergence is the expected outcome of first contact, and recording it is part of the milestone.**

---

## 4. Independent tracks

### B. Session storage — SQLite

> § *Session storage — SQLite*. Depends on nothing; the RPCs that expose it depend on A3.

**What.** A `HistoryStore` implementation over SQLite3 (already a `find_package` dependency, currently unused), plus session metadata for `ListSessions`.

**Acceptance criteria**
- Messages persist and reload byte-identically, including tool-call blocks.
- A crash mid-turn leaves a readable transcript (no partial-write corruption).
- Schema version recorded; a database newer than the binary is refused (§ *Startup sanity check*).

**Test cases** (`tests/session_store_test.cpp`)
- [ ] Round-trip every `Message` content-block variant, including `ToolUseBlock`/`ToolResultBlock` pairs.
- [ ] Reopen after an abrupt close; assert no partial message is returned.
- [ ] Schema-version mismatch is refused with a clear error.
- [ ] Paging: `GetMessages` by `seq` range over a large transcript stays bounded in memory.

### F. Startup sanity check

> § *Startup sanity check — `src/server/config/sanity_check.{hpp,cpp}`*. Independent; `main.cpp:114` is already staged for it.

**How.** `StartupSanityCheck::RunAll()` returns `vector<CheckResult>` and **runs every check regardless of earlier failures** — it is a linter, not a fail-fast validator, so a misconfigured setup yields one complete diagnostic instead of a fix-one-rerun-find-the-next loop. A cold start with nothing configured is one more failing check with a clear reason, not a bespoke onboarding wizard.

**Acceptance criteria.** The checks enumerated in that section: every config file parses; a usable `providers.json` entry exists with a resolvable key *and* a registered provider kind; each `mcps.json` entry's `command` resolves on `PATH` (stdio) or `url` is well-formed (http); `project_root` exists and is writable; the SQLite database is accessible and not newer than the binary; each discovered plugin's ABI tag matches; `max_concurrent_subagents` is sane; **no registration-name collisions (fatal)**; every `command_aliases` key resolves to something registered.

Bundled-plugin failures are Fatal, user-plugin failures Warning.

**Test cases** (`tests/sanity_check_test.cpp`)
- [ ] Each check fires on its own broken fixture and passes on a good one.
- [ ] **Two independent failures both appear in one `RunAll()` result** — the linter property is the whole point and is the easy one to regress into fail-fast.
- [ ] Bundled-vs-user severity split is respected.
- [ ] A name collision is fatal — this is the rule that makes tool replacement deliberate (§ *Name collisions and aliases*).
- [ ] Unknown `allowed_skills` entries are reported (currently silently dropped by design — `agent_preset_registry.cpp` notes "a future sanity_check makes them loud").

### H. Production compactor

> § *Context compaction*. Both sides exist; only the wiring is missing.

**Acceptance criteria**
- Summarises via the provider using `prompts/compression.md`, or a preset's `compression_prompt` override.
- **Never splits a `tool_use`/`tool_result` pair** — already implemented and tested as boundary snapping; the production path must route through it, not around it.
- Compaction failure degrades to "no compaction" rather than losing the transcript.

**Test cases**
- [ ] Real compactor against `FakeLlmProvider`; assert the boundary invariant still holds end to end.
- [ ] Provider error during compaction leaves history untouched.
- [ ] Preset-level `compression_prompt` override is used when present.

### I. `SessionObserver` registry

> § *`SessionObserver` — observe and react*. Schedule alongside A2, since `OnSessionEnded` needs a session to end.

**Acceptance criteria.** `tools-bash` registers an observer whose `OnSessionEnded` calls `BashStore().Erase(session_id)`; the cwd/env map no longer grows for the process lifetime (`bash_session_state.hpp:21`).

**Test cases**
- [ ] Ending a session erases exactly that session's bash state and no other's.
- [ ] A throwing observer does not take down session teardown.

---

## 5. Dependent tracks (after A)

### C. `/`-command system

> § *`/`-command system — `src/server/commands/`*, § *Name collisions and aliases*.

Core registry plus `Command` → (`LlmCommand`, `HarnessCommand`) hierarchy; `CommandContext` borrowing session-owned references. Read-only `HarnessCommand`s dispatch to a short-lived task — *not* inline on the reader thread, which must never block; everything else queues for the worker at a turn boundary.

Fills the empty `commands-core` plugin and unblocks `/restart-shell` in `tools-bash` (`plugin.cpp:62`).

- [ ] Read-only commands execute without blocking the reader.
- [ ] Non-read-only commands land at a turn boundary, never between a `tool_use` and its `tool_result`.
- [ ] Alias resolution and collision rules match § *Name collisions and aliases*; permission keys are never aliases.
- [ ] `/restart-shell` resets the same map `restart: true` does — proving plugin-owned state has one owner.

### D. Subagents

> § *Subagents*. Only `kSpawnSubagentToolName` and the no-nested-subagents exclusion exist today.

A subagent is a `Session` with a parent pointer — the same entity, not a second implementation. Blocking and non-blocking paths both route through one `AgentLoop::RunTurn`.

- [ ] Blocking subagent returns its final text as the parent's `ToolResultBlock`.
- [ ] Non-blocking completion is injected at a turn boundary (`QueueInjectedMessage` already exists for exactly this).
- [ ] Terminating a blocking subagent unblocks the parent with an error result rather than hanging (§ *Session lifetime*).
- [ ] A subagent cannot spawn subagents — already enforced in `agent_loop.cpp:85`; assert it end to end.
- [ ] `clamp_run_mode` holds: a child is never more permissive than its parent.
- [ ] Consent-grant inheritance decided and tested — plan leans copy-at-spawn, since re-prompting for every binary makes subagents unusable (see § *Consent scoping*, non-interactive note).

### E. TUI client

> § *TUI rendering*, § *TUI layout*, § *Tab states and attention*, § *Message-kind rendering*, § *@-mention file references*.

Largest remaining surface: FTXUI layout, md4c markdown rendering, tree-sitter highlighting, themes, session picker, running-tools panel, permission modal, @-mentions (`SuggestPaths`). Depends on A3 for every RPC it uses and on B for scrollback.

The permission modal is where consent scoping becomes real UI — and where the **wording obligations** in § *Consent scoping* must be honoured: a grant is blanket permission for a binary, and disclosure is a lower bound.

- [ ] Permission modal renders `PermissionDisclosure` including per-binary session grants.
- [ ] Streaming tokens render without full-buffer reflow at 512k–1M context.
- [ ] Tab bar: colour + single `!` attention glyph only (no metadata clutter).
- [ ] Scrollback pages via `GetMessages` without competing with live turn traffic.

### G. MCP client

> § *MCP client — `src/server/mcp/`*. cpp-mcp is pinned and fetched but never referenced; only permission plumbing exists.

Lazy connection on first MCP tool call. Tools register as `mcp__<server>__<tool>` — `mcp_server_of` and the per-server permission chain are already built and tested.

- [ ] Tools from a stdio MCP server appear as `mcp__<server>__<tool>` and execute.
- [ ] Per-server permission policy governs them (already tested in isolation — assert it end to end).
- [ ] A dead/hanging MCP server degrades to a tool error, never a hung worker.
- [ ] Connection is lazy: no MCP process spawns until a tool is actually called.

---

## 6. Cross-cutting standards

Applies to every milestone. These are existing project practice, recorded so they survive contributor turnover.

- **Invariant tests must be proven load-bearing.** Delete the invariant, watch the test fail, restore. Already applied to the sort comparator, `MaxTokens`, the grant conjunction, the vacuous-truth guard, and every skill-registry diagnostic. A test that passes with the fix removed is not a test.
- **Verify by execution, not inspection.** Plugin linkage was confirmed with `nm -u`/`otool -L`; the ABI mismatch was demonstrated against a running server; the bash hangs were measured (8008 ms → 0 ms). Claims about behaviour need a run behind them.
- **Zero warnings.** `-Wall -Wextra` on all first-party targets; the tree is clean and must stay so.
- **`ARCHITECTURE.md` is updated in the same change that diverges from it.** The bash rework's core finding was a restriction that outlived its rationale because the doc was updated without re-deriving the rule.
- **Test tiers** (§ *Testing*): pure logic → agent loop against `FakeLlmProvider` → tools against a temp `project_root` → end-to-end over a real socket.

---

## 7. Known gaps carried forward

Not scheduled, recorded so they are decisions rather than oversights.

- **Server-wide fd hygiene** — no `CLOEXEC` discipline on gRPC/SQLite/log fds; every bash command inherits them. The bash pipes are handled; the rest is not.
- **`tree-sitter-bash` for command disclosure** — the hand-rolled scanner is conservative and its failure mode is safe (fewer grants offered). Upgrading raises the hit rate, not the safety floor. The grammar is already a planned client dependency.
- **Unknown `allowed_skills` entries are silently dropped** — correct behaviour for resolution, but should be *reported* by F.
- **Backend `usage: 0`** from some OpenAI-compatible proxies — no fallback estimate designed (§ *Deferred*).
- Everything under § *Deferred / open items*: Anthropic provider, Windows, `llamacpp-kv`, parallel tool dispatch, session detach, tab-bar overflow.
