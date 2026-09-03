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

## Phase 2 — Structural moves (clean cuts, no compat shims) — DONE

Shipped as 5 independently-buildable, `ctest`-green commits. The doc's
consumer list for the HTTP/SSE move was incomplete, and its framing of the
`HistoryStore` fix blamed the wrong include — both corrected below.

- **`rpc/event_queue.hpp` → `common/`** (`minimal/common/event_queue.hpp`,
  namespace `minimal::common`). Generic concurrency primitive, header-only
  (no `.cpp`, no CMake source-list change needed). Consumers updated:
  `hooks/hook_dispatcher.hpp:69`, `agent/session_observer_registry.hpp:47`
  (doc had this at line 46), `tests/event_queue_test.cpp`.
- **`config/fd_hygiene` → `common/`.** Process-level POSIX utility, nothing
  configurational about it. Consumers: `main.cpp` (include only — the call
  site's position, right before the gRPC server is constructed, is an
  ordering constraint independent of the header's location, and did not
  move), `tests/fd_hygiene_test.cpp`.
- **HTTP/SSE out of `llm/`** → new `src/server/net/` module, namespace
  `minimal::net`. `http_client` (incl. Phase 1's `HeaderNameEquals`) +
  `sse_parser` move; `llm/` keeps only genuine LLM concerns (chat, message,
  provider, model_catalog, token_accounting). **The doc's consumer list was
  incomplete** — verified and updated: `mcp/mcp_connection.cpp`
  (`HttpTransport`), tools-web, cost-tracker, provider-openai (all named in
  the original list), plus **provider-anthropic** (same usage shape as
  provider-openai, missing from the original list) and
  **`plugins/include/minimal/plugins/plugin_abi.h`** (the frozen ABI
  umbrella header that directly includes both and is how every plugin sees
  them transitively — also missing from the original list, and the one
  file where missing it would have silently broken every plugin).
- **`HistoryStore` interface flipped to the session side**, now
  `session::HistoryStore` (not merely relocated while staying in
  `minimal::agent` — the interface and its `SqliteHistoryStore`
  implementation now share a namespace). **Doc correction**: the doc blamed
  `agent/session_registry.cpp:11`'s include of `session/sqlite_history_store.hpp`
  as "the circular half" needing to go away. That include is legitimate
  (agent/ constructing a concrete `session::SqliteHistoryStore` for
  `Session`) and did not change. The actual, narrower fix: only
  `agent/history_store.hpp`'s definition moved, removing `session/`'s one
  remaining include of an `agent/` header.
- **`synthesize_missing_tool_results` → own file**
  (`agent/transcript_repair.{hpp,cpp}`). Confirmed zero coupling to
  `snap_compaction_boundary` (no shared state or helper) — it was torn-tail
  resume repair squatting in `compaction_boundary.cpp:63-105`, not
  compaction-boundary logic. Test cases split out of
  `compaction_boundary_test.cpp` into `transcript_repair_test.cpp` at their
  pre-existing seam.

---

## Phase 3 — Config ownership — DONE

Shipped as 8 independently-buildable, `ctest`-green commits (step 2 below
was a decision with no code change). Unlike Phases 1-2, this phase's charter
explicitly allowed user-visible behavior changes — three landed, each called
out below and in its commit message, never silent. The doc's original line
numbers were mostly stale (`main.cpp` had shifted from Phase 2 edits), and
two items were structurally bigger or smaller than described — corrected
below.

1. **`LoadJsonConfig<T>` foundation**, `config/json_config_loader.hpp`. All
   six `Load*Config` functions collapse onto one template parameterized by
   `AbsencePolicy` (throw vs. default-on-missing) and `EmptyFilePolicy`
   (malformed vs. absent) — the real divergence turned out finer than the doc
   claimed: only `hooks.json` treats an empty-but-present file as absent;
   `mcps.json`/`channels.json` treat it as malformed, same as
   settings/permissions/providers. **Behavior change**: `hooks.json`'s
   malformed-JSON case now throws the same wrapped `std::runtime_error` every
   other loader already threw, instead of a raw `nlohmann::json::parse_error`
   (a test was locked to the old raw type; updated in the same commit).
   Deliberately does not read via `common::ReadFileOrEmpty` (Phase 1) — that
   helper collapses "could not open" and "opened but empty" into one result,
   erasing exactly the distinction the two policies exist to preserve.
2. **settings.json's two early duplicate reads hoisted into one.** Logging
   setup (stage 1) and the tools-slice extraction (stage 2) each read the
   file independently, ahead of the sanity check's own authoritative parse
   (stage 3) — both now source one hoisted `LoadSettingsConfig` call. Added
   `ToolsSettings::raw` (the full parsed `"tools"` object, verbatim) so the
   tools-slice site could stop bypassing `SettingsConfig` entirely without
   narrowing its documented "forwarded verbatim, no schema imposed" contract.
   **Behavior change, called out in the commit**: the hoisted preload uses
   full validation, so a schema violation *anywhere* in settings.json (not
   just in `logging`/`tools`) now leaves both early sites at their
   absent-file defaults — in practice inconsequential, since the sanity check
   aborts the process at stage 3 regardless. Stage 3's own read is
   deliberately left alone (see item 3's disposition below).
3. **`sanity_check` full injection — deferred, not shipped.** Verified the
   doc's premise was only half right: `main.cpp`'s own comment claimed
   settings/permissions/providers/mcps were all harvested from the sanity
   check, but `sanity.providers()` and `sanity.mcps()` were **never called**
   — `providers.json` and `mcps.json` were independently reloaded elsewhere
   (see item 8), and `permissions.json` was harvested *and* redundantly
   reloaded. Fixed those consuming-side redundancies directly (items 6, 8)
   instead of inverting `StartupSanityCheck` into an injectable validator —
   deferred as disproportionate: 87 call sites across 44 `TEST_CASE`s in
   `tests/sanity_check_test.cpp` rely on it loading files itself, for a
   blast radius that buys nothing once the actual redundancy is gone.
4. **Env-lookup semantics unified onto "empty = unset."** Three independent
   `getenv` paths existed (not two): MCP `${VAR}` interpolation
   (`mcp_client_manager.cpp`'s lookup lambda) treated a set-but-empty
   variable as resolved to `""`; `providers.json`'s `api_key_env`
   (`providers_config.cpp`) and `src/common/config_paths.cpp`'s
   `$HOME`/`$XDG_*` resolution already agreed on "empty = unset." Added
   `common::LookupEnv(name)` as the one shared primitive; all three paths now
   use it. **Behavior change**: an MCP `${VAR}` referencing a set-but-empty
   variable now fails to resolve instead of substituting `""` (regression
   test added). Extending `${VAR}` syntax to `providers.json` was evaluated
   and **deferred** — no concrete driver beyond symmetry; `api_key_env`
   already gets equivalent semantics via `LookupEnv` without new syntax.
5. **`ProvidersConfig` split**, internal only. Its own mutex doc-comment
   already stated the exact boundary; split into `ImmutableCatalog`
   (`models_`/`providers_`, no lock) and `DiscoveryCacheState` (everything
   the mutex guards). **Kept as one public class, not two**: `operator=`,
   `Resolve`, `ResolveDiscovered`, `PublishLocked`, `ModelRefs`, and
   `RefreshDiscovery` all read/write both groups, so a two-public-class split
   would be non-mechanical for no caller-facing benefit — every construction
   site only ever goes through `ParseProvidersConfig`/`LoadProvidersConfig`
   regardless. `BuildDiscoveredLocked`, which touched neither group despite
   its name, became a free function.
6. **Policy out of `main.cpp` — one item kept in place, one moved.** The
   model-discovery "adapter" turned out to be a ~15-line `DiscoveryHook`
   lambda, not the discovery cache itself (that already lived entirely in
   `ProvidersConfig`, see item 5) — `AgentPresetRegistry` doesn't hold a
   `ModelCatalogRegistry&` today, so moving it would mean a new method to
   hold 15 lines. **Left in `main.cpp`.** The hooks-warning dedup *did* move:
   `check_hooks()` re-implemented `HookDispatcher::Configure`'s point
   validation by hand and had silently drifted — `Configure` emitted a
   "no filter" warning `check_hooks()` never produced, contradicting
   `main.cpp`'s own comment that `Configure` "adds nothing beyond the
   check." Extracted into `HookDispatcher::DescribeActionWarnings()`, shared
   by both. **Behavior change**: the sanity check now also reports
   filter-less hooks.json actions (one test's fixture updated).
7. **Bounded-queue policy — partially shared, mostly deferred.**
   `EventBroadcaster::Deliver`'s per-subscriber eviction and `Publish`'s
   zero-subscriber backlog eviction (a **third, previously unflagged**
   in-file duplicate of the same rule) now share one `EvictOldestUntilFits`
   helper. **Deliberately not unified** with `common::EventQueue`'s
   `Fits`/`Accept` (moved to `src/common/` in Phase 2): `EventQueue` blocks
   or refuses on overflow and never evicts — a guarantee two unrelated
   production consumers (`HookDispatcher`, `SessionObserverRegistry`) depend
   on — while `EventBroadcaster` always evicts and never blocks. The two
   classes' overflow *actions* are genuinely different QoS models; only the
   `Fits`-shaped bounding *predicate* is byte-for-byte identical, and
   templating just that across both was judged not worth the coupling risk
   for the savings.
8. **`mcps.json`/`permissions.json` double-parses eliminated** (found during
   item 3's verification, not originally itemized). `main.cpp` built
   `McpClientManager` from an independent `LoadMcpsConfig` call instead of
   `sanity.mcps()` — fixed with a pure call-site change.
   `AgentPresetRegistry`'s constructor independently reloaded
   `permissions.json` despite `sanity.permissions()` already being computed
   and used elsewhere — fixed with an additive 4-arg constructor overload (the
   existing 3-arg one is unchanged, still used by 30+ test call sites).
   `providers.json`'s double-parse is **intentionally left alone** —
   `main.cpp`'s own comment documents it as deliberate: the live,
   discovery-hooked instance `AgentPresetRegistry` builds is not
   interchangeable with the sanity check's un-hooked one.

---

## Phase 4 — Full event extraction (the centerpiece) — DONE

Shipped as 4 independently-buildable, `ctest`-green commits (steps 5-9 of the
original plan landed as one combined commit — the compiler forces it: slimming
`SessionChannels` breaks every direct consumer, including 6 test files, in
the same edit that makes the type change). Design intent survived intact
(`agent::SessionEvent`, `rpc::EventTranslator`, wire format unchanged,
`headless_e2e_test` passes unmodified); several of the plan's mechanics
turned out different once implemented — corrected below.

- **Vocabulary and translation** (`agent/session_event.hpp`,
  `rpc::ToProto(session_id, SessionEvent)`): the plan's alternative list was
  half right. `TurnSummary` is **not** the `turn_completed` alternative —
  `Session::EmitTurnCompleted` takes `(StopReason, Usage, hit_max_turns)` and
  feeds `TurnCompletedEvent`; `TurnSummary` is a *different* object
  (`first_seq`, `tool_names_invoked`, no `hit_max_turns`) assembled
  separately for `SessionObserver::OnTurnCompleted` — a new `TurnCompleted`
  struct is the wire-shaped one, `TurnSummary` untouched. The doc's single
  `ErrorEvent{category, message, http_status, is_retryable}` also overfit:
  9 of the 10 error-emitting sites in `session.cpp` hardcode
  `category=UNSPECIFIED`/no `http_status`/`is_retryable=false` — only
  `message` ever varies — so those became a minimal `SessionError{message}`,
  leaving `llm::LlmError` (already in the variant) as the one alternative
  that carries a real category. `commands::CommandResult` alone cannot cross
  to the wire either: `CommandResult.request_id` needed for the proto lives
  on `QueuedCommand`, not on the outcome a `Command` returns, so a
  `SessionCommandResult{request_id, CommandResult}` wrapper carries both.
- **`SessionChannels` split**: the domain half (`InjectionQueue`,
  `background_tasks_live`) stays in `agent/`, unchanged; the transport half
  (`EventBroadcaster`, `PendingRequests`) moves to a new
  `rpc::TransportChannels`, reached only through `rpc::EventTranslator`
  behind `SessionParams::event_sink`/`transport_handle` (a type-erased
  `shared_ptr<void>` — deliberate, so `agent::Session` never names the
  concrete `rpc::` type it points at). `SessionEventSink` ended up
  `bool(const SessionEvent&)`, not the originally-sketched `void`: several
  session.cpp sites warn on a dropped event, and the stream-event path uses
  the return value to abort an in-flight provider request on the race
  between an `IsCancelled()` check and `Terminate` closing the broadcaster —
  a void sink would have silently dropped both behaviors.
- **`rpc::session_wiring` ended up two-phase, not one factory**: a real
  timing constraint the plan missed — `ApplyPermissionAdvisor` (the advisor
  decision) runs at `session_service.cpp`/`subagent_manager.cpp`'s
  composition sites, strictly *before*
  `SessionRegistry::CreateResolved`'s funnel resolves
  `unattended_prompt_timeout`. The default `WirePermissionChecker` and the
  always-built `WireConfirmationBroker` both need that resolved value, so a
  second function, `FinishSessionWiring`, runs from `CreateResolved` itself
  right after the resolution, building whichever of
  {transport, default checker, confirmation broker} the early phase left
  unbuilt. This is what actually kills the three-way duplicate composition
  (`session_service.cpp`, `subagent_manager.cpp`, and `Session`'s own former
  ctor fallback) — not two of three, as an earlier draft of this doc
  described.
- **`Session` emission cutover**: all 21 `session.cpp` sites and
  `subagent_manager.cpp`'s `subagent_started` now build a `SessionEvent` and
  call the sink (`Session::EmitEvent`), preserving every invariant the
  emission-surface survey flagged (`SessionEnded` before closing the
  transport, `MaybeEmitNameFromStore` before `EmitTurnCompleted`,
  `SubagentStarted` before the child's first event) and the exact per-site
  warn-on-drop behavior each one had. `RejectCommand` did **not** dissolve
  into `session_service` as the plan proposed — `session_service.cpp` calls
  it as an ordinary method on a live `Session` at three sites, and
  duplicating command-result construction three times would have fought the
  phase's own dedup goal; it now emits `SessionCommandResult` like every
  other command-result site instead.
- **Cutting the cord**: `agent/session.hpp` and `agent/session.cpp` depend on
  `rpc/` **nowhere** now — not just the four originally-named includes, the
  full `v1/session_service.pb.h` include, and every `rpc::` type
  (`EventBroadcaster`, `PendingRequests`, `WirePermissionChecker`,
  `WireConfirmationBroker`) it used to construct or return.
  `Session::queue()`/`pending()`/`Subscribe()`/`Unsubscribe()` are gone;
  `session_service.cpp` owns `Subscribe`/`Unsubscribe` directly against the
  transport (a `Transport()` helper casting the opaque handle back to
  `EventTranslator`), paired with new `Session::NotifyAttached()`/
  `NotifyDetached()` so `has_subscribers()`/`ever_attached()` keep working
  off a mirrored counter. `WireConfirmationBroker` got the injection seam it
  never had (`SessionParams::confirmation_broker`) as a forced side effect —
  `Session`'s ctor can no longer construct one itself.
- **Fail loud, not silent**: `Session`'s constructor (`RequireWiring`) throws
  immediately if any wiring field is missing, rather than surfacing later as
  a dropped event or a `std::bad_function_call` from inside a lambda. Every
  production session gets these from `FinishSessionWiring`; the 6 test files
  that construct a `Session` directly (bypassing the registry) call it
  themselves.
- **Answer direction de-proto'd**, landed as a separate commit:
  `PendingRequests::PermissionAnswer` was a serialized-proto string, parsed
  on the worker via `ParseFromString`. It is now
  `{tools::PermissionDecision, std::vector<std::string> granted_keys}`,
  decoded by the reader thread from the already-typed `v1::PermissionResponse`
  it holds (no re-serialize-then-reparse round trip). Grant-key filtering
  against the request's disclosure stays on the worker
  (`wire_permission_checker.cpp`) — only the `WirePermissionChecker` that
  built the original request's disclosure can filter against it; the reader
  has no access to that state. This removed
  `wire_permission_checker_test.cpp`'s "an unparseable answer denies rather
  than crashing" case — that failure mode cannot occur at this layer
  anymore; the equivalent coverage already exists at
  `proto_convert_test.cpp`'s "an unspecified permission decision denies".
- **Gate**: `headless_e2e_test.cpp` has **zero** diff across the whole phase
  and its 46 cases pass; full suite (1507 cases) green; a ThreadSanitizer
  build (`-fsanitize=thread`, vcpkg binary cache hit — no dependency
  rebuild needed) of every `[session]`/`[wire_permission]`/
  `[event_translator]`/`[broadcaster]`/`[pending_requests]`/
  `[session_registry]`/`[subagent]`/`[headless]`/`[resume]` case (257 cases,
  ~41k assertions) reports zero data races.

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
- `StartupSanityCheck` staying a composition root for settings/permissions/
  providers/hooks (Phase 3): the actual redundancy this would have fixed
  (`providers.json`/`mcps.json` never harvested despite being parsed, a
  redundant `permissions.json` reload) is already gone at the consuming side
  — `sanity_check_test.cpp`'s 87 call sites across 44 cases all rely on the
  "write file(s), call `RunAll()`, assert" idiom, and inverting it now buys
  nothing further.
- Bounded-queue policy between `common::EventQueue` and `EventBroadcaster`
  (Phase 3): confirmed genuinely divergent overflow QoS (block-and-never-evict,
  relied on by `HookDispatcher`/`SessionObserverRegistry`, vs.
  always-evict-never-block) — only the `Fits`-shaped bounding predicate is
  identical, and sharing just that risked coupling two deliberately different
  models for marginal savings. `EventBroadcaster`'s own internal duplicate
  (`Deliver` vs. `Publish`'s backlog eviction) was fixed instead.

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
| 2 Moves | M | Low-Med | build + ctest after include churn | DONE |
| 3 Config | M | Medium | config/sanity/e2e tests | DONE |
| 4 Event extraction | **L** | Medium | `headless_e2e_test` unmodified-pass | DONE |
| 5 Slimming | M-L | Medium | per-step ctest | |
