# Server Core Fixes — Remediation Plan

Source: full review of `src/server/` (~26.7k lines) covering bad architecture
decisions, duplicate code, and duplicate abstractions. All findings verified
against the code with file:line references. Decisions taken at planning time:

- **Dependency inversion depth: FULL event extraction.** `Session` emits domain
  events; `rpc/` translates to `v1::ServerEvent`. The proto stays as the wire
  vocabulary only.
- **No backward compatibility.** Pre-release, no third-party plugins exist.
  Header/namespace moves are clean cuts; bundled plugins updated in the same
  commit. No forwarding headers or aliases.
- **All user-visible behavior changes are acceptable** (config parse
  strictness, env-var semantics, proto validation errors).
- **Delivery: phased merges.** One branch per phase, each independently green
  (build + full test suite), merged in order.

Verification per phase: `cmake --build` + full `ctest` in `build/`; **tsan**
build for thread-sensitive phases (0, 4); **asan** for Phase 0.1.
`headless_e2e_test` passing **unmodified** is the hard gate for Phase 4
(wire format must not change).

---

## Phase 0 — Bugs (small, each with a regression test)

| # | Fix | Reference | Test |
|---|-----|-----------|------|
| 0.1 | Reaper iterator bug: `it = sessions_.erase(it); quiescent_since_.erase(it->first);` erases the *next* session's timestamp (UB at `end()`), leaks the doomed session's entry. Save the key before erasing, mirroring the correct guard-expired pattern ten lines above. | `src/server/agent/session_registry.cpp:448-449` | `session_registry_test`: reap two detached sessions, assert both TTL clocks behave |
| 0.2 | `SuggestPaths` is the only RPC (of nine) without `EnsureAuthenticated` — unauthenticated filesystem path suggestions. Add the guard. | `src/server/rpc/session_service.cpp:818` | `auth_test`: unauthenticated `SuggestPaths` → `PERMISSION_DENIED` |
| 0.3 | Concurrent `Session::Terminate` double-joins `worker_` (UB): the loser of the `terminated_.exchange` race still calls `join()`. Only the winner joins. | `src/server/agent/session.cpp:766-813` | tsan run of `session_test` + `subagent_test` (concurrent Terminate vs last-ref release) |
| 0.4 | `AuthProvider::provider()` reads `provider_` without `mu_` (write path is guarded, read path isn't). | `src/server/rpc/auth_provider.hpp:41` | `auth_test` race case |
| 0.5 | `SessionObserverRegistry::WaitForIdleForTesting` busy-waits (`yield()` loop) → condition variable. | `src/server/agent/session_observer_registry.cpp:123-127` | existing observer tests |

---

## Phase 1 — Mechanical dedup (no behavior change) — DONE

Shipped as a sequence of independently-buildable, `ctest`-green commits.
Several of this section's original entries were stale (wrong function names,
wrong line numbers, a claimed third duplicate that did not exist) or, on
closer reading, not safe as a plain "keep one, delete the other" mechanical
dedup — corrected below; see the branch history for the exact per-item
implementation notes.

- **`ReadFileOrEmpty` ×3 byte-identical** → `minimal::common::ReadFileOrEmpty`
  in `src/common/include/minimal/common/read_file.hpp`. Sites: `main.cpp:55`,
  `config/sanity_check.cpp:26`, `agent/agent_preset_registry.cpp:17` (doc had
  this one off by one). The 3 "variant" sites (`skill_registry.cpp:18`,
  `hooks_config.cpp:95-103`, `agent_preset_frontmatter.cpp:98-104`) were
  **not** the same function — each has distinct, load-bearing error semantics
  (optional vs. throw vs. empty-as-absent) — and were left alone; they belong
  to Phase 3's `LoadJsonConfig` work.
- **`DescribeModels`/`DescribeDiscovery` duplicated** → shared free functions
  next to `ProvidersConfig` (`config/providers_config.hpp`/`.cpp`). Sites:
  `session_registry.cpp:18-37` vs `agent_preset_registry.cpp:79-98` (the
  latter was actually named `DescribeAvailableModels`, not `DescribeModels`).
- **`SessionDatabase` raw-row collect/decode loop ×3** → shared `RawRow`
  struct, `CollectRows` (collect half) and `DecodeRow`/`DecodeRows` (decode
  half) helpers. Sites: `GetMessagesPage`, `LoadMessages`, `LoadMessagesFrom`.
  `GetMessagesPage`'s collect loop has `limit`-based early-break/`has_more`
  logic the other two don't share — its collect loop was deliberately left
  bespoke (only `DecodeRow` unified across all three); a `limit+1`/post-loop
  `has_more` rewrite is optional future cleanup, not this phase.
- **`ParseStringMap` (incl. CR/LF rejection) duplicated** → hoisted from
  `mcps_config.cpp` (already the generalized version) to
  `config/include/minimal/config/parse_util.hpp`; `channels_config.cpp`'s
  hand-inlined `env`-parsing block (it had no function of its own) now calls
  the shared one.
- **`mcp__` naming convention in 3 modules** → `mcp/naming.hpp` gained
  exposed `kMcpPrefix`/`kMcpSeparator` constants and a new `McpServerOf()`
  (the inverse-parse `tool_executor.cpp`'s `mcp_server_of()` used to
  implement locally — additive, since naming.hpp had no inverse direction
  before). `tool_registry.cpp`'s 3 raw `"mcp__"` literals now use the
  exposed constants.
- **Byte-identical SQLite helper cluster** (found during this phase, not in
  the original list) → `NowMillis`, `ThrowSqliteError`, `ExecChecked`,
  `struct Stmt`, and the migration-rollback `catch (...)` block, shared
  between `plugins/plugin_storage.cpp` and `session/session_database.cpp`,
  moved to `src/common/include/minimal/common/sqlite_util.hpp`.
- **Run-mode wire strings defined twice** → `config::ToString(PermissionRunMode)`
  added to `permission_codec.hpp`/`.cpp` alongside the existing
  `ParsePermissionRunMode`; `session_service.cpp`'s local `RunModeString`
  deleted.
- **`IEquals` vs `equals_ci`** → `llm::HeaderNameEquals`, exposed publicly in
  `llm/http_client.hpp` next to `FindHeaderCI`; `mcp_connection.cpp`'s local
  lambda deleted in favor of calling it directly.
- **Duplicate comment block** `http_client.cpp:353-362` — the orphaned first
  copy (353-356, preceding nothing it described) removed.
- **`TextOf` duplicated (2 signatures) + 3 differently-scoped byte-sum
  loops** → unified `llm::TextOf(span<const ContentBlock>)` +
  `llm::TextOf(const Message&)` in `llm/message.hpp`. The 3 sum loops were
  **not** unified: `agent_loop.cpp`'s two only count `TextBlock`,
  `context_compactor.cpp`'s `estimate_tokens` also counts `ThinkingBlock`/
  `ToolUseBlock`/`ToolResultBlock` — collapsing them would have silently
  widened what `CapToolResult`/`ElideLargestToolResult` count, a real
  behavior change. Left as three separate, unequal-scope loops.
- **MCP channel-skip policy mirrored in main** → `McpClientManager::EnabledChannelNames()`
  added; `main.cpp` now registers with the signal router only the channel
  names the manager actually built a handle for, instead of re-deriving the
  same enabled/reserved/collision rules from raw config. **Found a real
  latent bug while verifying this was safe**: `main.cpp`'s old collision
  check (`sanity.mcps().Find(name) != nullptr`) matched a same-named
  mcps.json entry regardless of `enabled`, while the manager's own collision
  only ever came from *enabled* entries occupying `handles_` — so a disabled
  mcps.json entry silently blocked an otherwise-working channel of the same
  name from ever being registered for subscription. Fixed as part of this
  phase (small, regression-tested: `mcp_signal_channel_test.cpp`), not
  deferred, since the correct behavior fell out of asking the manager
  directly rather than re-deriving its rules.
- **Command-refusal exact strings copy-pasted** → `commands::UnknownCommandMessage`/
  `BusyRefusalMessage` in `command_dispatcher.hpp`/`.cpp`, used by both
  `command_dispatcher.cpp` and `session_service.cpp`'s `DispatchCommand`
  **without** merging their control flow — the two have different check
  ordering, and `DispatchCommand` has a read-only thread-pool path and a
  "no registry configured" fallback with no counterpart in `Dispatch`.
  `Dispatch`'s `busy` parameter is dead code in production today (both
  callers hardcode `false`) — left alone, flagged for a separate cleanup
  decision, not removed under a mechanical-dedup banner.
- **`QueueInjectedMessage` on both Session and AgentLoop — corrected: not a
  real duplicate.** `Session::QueueInjectedMessage` (load-bearing:
  `terminated_` guard + `turn_cv_.notify_all()` wakeup, used by every
  production caller via `shared_ptr<Session>`) and
  `AgentLoop::QueueInjectedMessage` (a bare queue push, exercised only by
  tests constructing a bare `AgentLoop`) merely shared a name. Deleting
  either in favor of the other, as originally written here, would have
  dropped production wakeup behavior. Fixed as a pure rename instead:
  `AgentLoop::QueueInjectedMessage` → `PushInjectedMessage`.
- **Deleted-overload cursor guard "copy-pasted"** — on inspection, already
  in the desired end state: `session_reader.hpp` carries the full
  explanation, `session_database.hpp` already just points at it
  ("See SessionReader::GetMessages for both of these"). No change needed;
  this list entry was stale.

**Deferred out of this phase** (not safe as a no-behavior-change mechanical
dedup; see "Explicitly not fixing" below):
- PATH-resolution / executable-check (`sanity_check.cpp`'s `CommandResolves`
  vs. `plugins/server/tools-bash/bash_tool.cpp`'s `ResolveOnPath`).
- `mcp_stdio_shim` reap-loop unification (`ReapChild` vs.
  `WatchChildDetached`) — moved to a Phase-0-style small-fix bucket.

---

## Phase 2 — Structural moves (clean cuts, no compat shims)

- **`rpc/event_queue.hpp` → `common/`** (`minimal/common/event_queue.hpp`). It
  is a generic concurrency primitive; `hooks/hook_dispatcher.hpp:69` and
  `agent/session_observer_registry.hpp:46` currently include from `rpc/`.
- **HTTP/SSE out of `llm/`** → new `src/server/net/` module, namespace
  `minimal::net`. `http_client` + `sse_parser` move; consumers updated:
  `mcp/mcp_connection.cpp` (HttpTransport), tools-web plugin, cost-tracker,
  provider-openai. `llm/` keeps only LLM concerns.
- **`config/fd_hygiene` → `common/`.** Process-level POSIX utility, nothing
  configurational about it.
- **`HistoryStore` interface flipped to the session side** — currently
  `session/sqlite_history_store.hpp:7` includes `agent/history_store.hpp` while
  `agent/session_registry.cpp:11` includes back into `session/` (circular
  directory dependency, interface on the wrong side). Interface moves to
  `session/`; agent/ depends on session/ only.
- **`synthesize_missing_tool_results` → own file** (`agent/transcript_repair`).
  Torn-tail repair is not compaction-boundary logic; it currently squats in
  `compaction_boundary.cpp:63-105`.

---

## Phase 3 — Config ownership

1. **`LoadJsonConfig(path, AbsencePolicy)` foundation** in `config/` + shared
   file-read. All six `Load*Config` functions collapse onto it with explicit
   per-file absence policy (currently: settings/permissions/providers throw;
   mcps/channels return empty; hooks treats empty-file-as-absent — silently
   divergent). Sites: `settings_config.cpp:241`, `permissions_config.cpp:70`,
   `providers_config.cpp:448`, `mcps_config.cpp:118`, `channels_config.cpp:157`,
   `hooks_config.cpp:95`.
2. **settings.json parsed three times** → parsed once in `main` via
   `SettingsConfig`. Remove `main.cpp:134` (logging load) and `main.cpp:262-264`
   (ad-hoc `json::parse` of the tools slice that bypasses the schema).
3. **`sanity_check` demoted from composition root to validator.** Today the
   845-line linter *loads* all configs (`sanity_check.cpp:110-182`) and `main`
   harvests them (`take_session_db()` etc.). After: configs load in `main`
   (the true composition root), `SanityCheck` receives them by injection.
4. **Env/secret mechanisms unified.** `${VAR}` interpolation (connect-time,
   `mcp/env_interpolation.cpp`) becomes the one mechanism, extended to
   providers.json (alongside the existing `api_key_env`, which keeps working);
   empty-value semantics unified to "empty = unset" (fixes
   `mcp_client_manager.cpp:205-208` treating empty as resolved, vs
   `providers_config.cpp:39` / `config_paths.cpp:12-14`).
5. **`ProvidersConfig` split**: immutable parse-result snapshot vs a separate
   `ModelDiscoveryCache` runtime service (mutex, gates, overlays, generations
   currently all in one class — `providers_config.cpp:88-326`).
6. **Policy out of `main.cpp`**: model-discovery fallback lambda
   (`main.cpp:422-438`) moves into `AgentPresetRegistry`/discovery cache;
   hooks-warning dedup (`main.cpp:469-478`) moves into `HookDispatcher`.
7. **Bounded-queue policy shared**: the byte-accounting / oversized-event /
   drain-then-close rules implemented twice (`event_queue.hpp:233-247` vs
   `event_broadcaster.cpp:196-227`) → one `BoundedItemPolicy` helper.

---

## Phase 4 — Full event extraction (the centerpiece)

**Design.** New `agent::SessionEvent` — a variant over *existing* domain types
(`llm::StreamEvent`, `ToolLifecycleEvent`, `tools::BackgroundTaskEvent`,
`llm::LlmError`, `commands::CommandResult`, `TurnSummary`) plus new small
structs for the rest — and a one-method `agent::SessionEventSink`. `rpc/` owns
an `EventTranslator` implementing the sink (domain → `v1::ServerEvent` via new
`proto_convert` mappers, feeding the existing `EventBroadcaster`, which keeps
carrying protos — it is transport). Wire format unchanged; plugins never touch
`ServerEvent` (verified), so the new vocabulary does **not** become plugin ABI;
the client is untouched.

Facts established by the emission-surface survey:
- 27 `v1::ServerEvent` construction sites in the server: 22 in agent/
  (21 in `session.cpp` via `MakeEvent` + 4 ctor-wired sink lambdas;
  1 in `subagent_manager.cpp:282-288`), 5 in rpc/ (legitimately transport-owned:
  `resync_required`, `prompt_resolved`, attach-time full `session_metadata`,
  and the two prompt-request events built by the wire checkers).
- `agent_loop.cpp` already emits zero protos — the pattern works.
- Permission/confirmation prompts need **no** domain event: `WirePermissionChecker`
  implements the domain port `tools::PermissionChecker` and
  `WireConfirmationBroker` implements `commands::ConfirmationBroker`; only
  their *ownership* moves out of Session (composition factory below).

Steps (each independently mergeable):

1. **Vocabulary**: `agent/session_event.hpp`. New payload structs:
   `SubagentStarted{child_session_id, task, blocking}`,
   `SessionMetadataChanged{name, agent_preset, model, permission_mode,
   context_window_tokens, partial}` (name-only variants use partial),
   `SessionEnded`, `ToolOutputDelta{tool_use_id, chunk}`,
   `ErrorEvent{category, message, http_status, is_retryable}`.
2. **Translation**: `proto_convert` gains `ToProto(SessionEvent) ->
   v1::ServerEvent` with the file's existing totality discipline (exhaustive
   switch, `static_assert` terminal else).
3. **Composition factory** `rpc/session_wiring.{hpp,cpp}`: builds the channels
   block, `EventTranslator` + broadcaster, `WirePermissionChecker`,
   `WireConfirmationBroker`, `PendingRequests`. One recipe consumed by both
   `session_service.cpp:90-103` (`ApplyAdvisor`) and
   `subagent_manager.cpp:197-206` — kills the checker-composition duplication
   (three sites today incl. `session.cpp:55-61`). `SessionParams` receives the
   sink + the two interfaces only.
4. **Session gutting**: all 21 proto sites in `session.cpp` become domain
   emission. Invariants that must survive (from the survey):
   - the channels `shared_ptr` remains the lifetime anchor for watcher threads
     that outlive Session (`session.cpp:107-129` capture semantics);
   - `background_tasks_live` counter bookkeeping (`session.cpp:132-145`)
     moves domain-side — Session counts STARTED/KILLED before forwarding; the
     reaper's quiescence rule depends on it;
   - ordering: `SessionEnded` published before `CloseAll`
     (`session.cpp:797-811`); metadata-before-`TurnCompleted`
     (`session.cpp:1045-1054`); `SubagentStarted` before the child's first
     event.
5. **Reader-thread refusals move rpc-side**: `RejectCommand`
   (`session.cpp:363-378`, invoked by `session_service.cpp:536/560/572`)
   dissolves — dispatcher returns the refusal reason as domain data,
   `session_service` builds the `command_result` event itself.
6. **`subagent_started`**: `SubagentManager` emits the domain event on the
   parent's sink.
7. **Answer direction de-proto'd**: `PendingAnswer`
   (`pending_requests.hpp:39-47`) carries a domain
   `tools::PermissionResponse` (parsed on the reader thread) instead of a
   serialized proto string — removes the last inward proto leak.
8. **Attach-time full metadata**: `session_service.cpp:991-1015` keeps
   building it from domain accessors; `preset_snapshot()` / `run_mode()`
   remain on Session.
9. **Cut the cord**: delete `agent/session.hpp:51-54` rpc includes +
   `session_service.pb.h`; `Session::queue()`/`pending()` exposure replaced by
   the sink interface. `session_service` owns `Subscribe/Unsubscribe` instead
   of borrowing them from Session.
10. **Tests**: `session_test`, `event_broadcaster_test` updated;
    `headless_e2e_test` passes **unmodified** (hard gate).

---

## Phase 5 — God-class slimming (builds on Phase 4)

- **`session_service.cpp` (1069 lines, ≥6 responsibilities)**: advisor
  composition and signal/MCP warm-up (`session_service.cpp:256-313`) move
  behind the registry funnel; Phase 4.3 removes most of the rest.
- **`CreateResolved` (250-line funnel, `session_registry.cpp:81-327`)** →
  builder steps: project-context → model resolution → resume/repair →
  construct → wire → publish. The resume seed-triple
  (`seeded_history, seed_first_persisted_seq, seed_head_is_summary` — three
  args that must agree) folds into one `ResumeSeed` struct; Session's 11-arg
  ctor shrinks accordingly (AgentLoop's 14-arg too where shared).
- **Two-phase init**: worker thread starts in Session's ctor
  (`session.cpp:262`) but wiring completes later
  (`session_registry.cpp:281-294`). All `set_*` wirings become
  constructor-injected; delete the duplicate `set_mcp_manager` channel
  (`session.hpp:419` — `SessionParams::mcp_manager` already exists).
- **Naming policy out of SQLite**: the first-user-text-becomes-tab-label rule
  currently lives inside `AppendMessage`'s transaction
  (`session_database.cpp:244-283, 505-516, 602-616`). Session computes the
  default name as a domain rule; the DB stores. Kills the post-turn
  `MaybeEmitNameFromStore` poll (`session.cpp:1118-1141`) — replaced by a
  `SessionMetadataChanged` emission at the same point in the turn.
- **Reaper decoupled from WAL**: `ReaperMain` calls
  `db_.MaybeRunPassiveCheckpoint()` every tick (`session_registry.cpp:476-481`)
  and the registry holds a concrete `SessionDatabase&` (`session_registry.hpp:193`).
  Registry takes a periodic-callback instead — unblocks fake-store testing.
- **`LoopConfig` cleanup**: defaulting (`compaction_summarizer ? ... : default`)
  moves from consumer (`session.cpp:251`) to producer; hooks stop being wired
  twice (`LoopConfig::hooks` at `agent_loop.hpp:79` AND `SessionParams::hooks`
  at `session.hpp:218`).
- **One shared `SessionReader`** instead of a stateless per-session instance
  (`session_registry.cpp:203` vs `session_support.cpp:17` proving one suffices).
- **`MentionResolverRegistry`** is a single-slot registry that warns and ignores
  a second registration (`mention_resolver.cpp:74-83`) — rename or make
  semantics honest (settable optional).
- **`CompactionStrategyRegistry`** (string-keyed registry with exactly one
  registration, whose bundle dissolves into loose `std::function`s at
  `main.cpp` before `AgentLoop` consumes it) → two named registries or one
  factory; strategy object survives to the consumer.
- **Structural twins**: shared rendezvous base for
  `WirePermissionChecker`/`WireConfirmationBroker` (same
  register-before-publish + timeout + fail-closed-decode skeleton,
  `wire_permission_checker.cpp:25-74` vs `wire_confirmation_broker.cpp:20-56`)
  — low marginal risk after Phase 4 touched both.
- **`proto_convert` strictness (behavior change)**: malformed client content
  blocks → `INVALID_ARGUMENT` instead of silent drop + log warning
  (`proto_convert.cpp:356-364, 386-391`).
- **Deferred/documented**: `PluginSubscribe` 20 ms polling thread per
  subscription (`session_service.cpp:1062-1064`, sync-API limitation);
  per-subscriber publish copies in `EventBroadcaster` (correct, throughput
  note only); `ModelCatalog` being OpenAI-`/models`-shaped (documented
  pragmatism).

---

## Explicitly not fixing (reviewed, judged correct)

- `wire_confirmation_broker` vs `commands/confirmation_broker` and
  `wire_permission_checker` vs tools permission model: correct port/adapter
  splits, not duplicates. Phase 4 completes the split by moving ownership.
- `event_queue` vs `event_broadcaster`: legitimately divergent QoS (blocking
  backpressure vs drop-oldest fan-out); only the duplicated bounding *policy*
  is shared (Phase 3.7).
- `hook_observer_bridge` vs `session_observer_registry`: clean adapter; the
  bridge reuses the registry's dispatch.
- The compaction four-file split (trigger/boundary/strategy/compactor): each
  file is one independently testable decision — coherent.
- `SessionObserverRegistry`'s drop-on-overflow QoS: correct for hooks/plugins
  (sessions keep running); must NOT be reused as the wire-event bus — Phase 4
  keeps it separate from `SessionEventSink`.
- PATH-resolution / executable-check (`config/sanity_check.cpp`'s
  `CommandResolves` vs. `plugins/server/tools-bash/bash_tool.cpp`'s
  `ResolveOnPath`, found during Phase 1): different return contracts (`bool`
  vs. `optional<path>` used as a permission-grant key) and the second copy
  lives in a plugin outside `src/server/`'s module boundary — not a safe
  same-signature extraction. (Phase 1's original list also claimed a third
  copy at `sanity_check.cpp:490-499`; that function checks directory
  writability, unrelated to PATH/executable resolution — doc error, not a
  real duplicate.)

---

## Small fixes queued (Phase-0-style, found during Phase 1)

- **`mcp_stdio_shim` reap-loop ordering**: `ReapChild` (`:166-184`, sync)
  checks-then-sleeps; `WatchChildDetached` (`:186-204`, detached thread)
  sleeps-then-checks — a real behavior difference (a child that already
  exited is detected with zero delay in one, one grace-period tick late in
  the other), not pure duplication. Standardizing on check-first (matching
  the sibling `AbortChild` idiom) is a small, desirable, explicitly-scoped
  behavior change — regression test + fix, same shape as Phase 0's items,
  not bundled into a no-behavior-change phase.

---

## Effort / risk summary

| Phase | Size | Risk | Gate | Status |
|-------|------|------|------|--------|
| 0 Bugs | S | Low | regression tests + tsan | DONE |
| 1 Dedup | M | Low | full ctest, zero behavior change | DONE |
| 2 Moves | M | Low-Med | build + ctest after include churn | |
| 3 Config | M | Medium | config/sanity/e2e tests | |
| 4 Event extraction | **L** | Medium | `headless_e2e_test` unmodified-pass | |
| 5 Slimming | M-L | Medium | per-step ctest | |
