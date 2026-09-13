# Findings Requiring Attention

Source-review findings for the `minimal` harness, verified against the tree at
commit `bf47a2d` (2026-09-12). Every item was confirmed by reading the cited
code; none are speculative. Companion document: [REVIEW_COMPARISON.md](REVIEW_COMPARISON.md)
(the comparative positioning); the full per-subsystem review was delivered
separately.

Severity ordering within each group is roughly "fix first" order.

## A. Likely real bugs (server)

### A1. EventQueue's terminal/drop-mode machinery is dead production code

`src/server/rpc/include/minimal/rpc/event_queue.hpp:5-21` admits it: the
`EventBroadcaster` took over the session's outbound queue, and "the
terminal/drop-mode machinery below has no non-test caller anymore." Production
users are two count-bounded task queues; `PushTerminal`, `AbortWaitingProducers`,
and the drop-mode latch are exercised only by `tests/event_queue_test.cpp`.

Why it matters: ARCHITECTURE.md documents the cancel-unwedge design (drop-mode
latching, `PushTerminal` carve-outs) at length as if it were the live outbound
path. It isn't — the broadcaster superseded it. The invariant-bearing surface is
kept deliberately, but it is dormant, and the architecture doc describes a
mechanism where the code path no longer runs.

Fix direction: either delete the machinery (and its tests) or rewrite the
ARCHITECTURE.md § Concurrency passages to describe the broadcaster's actual
mechanics. **(Resolved 2026-09-12: the terminal/drop-mode machinery and its
tests were deleted, and ARCHITECTURE.md § Concurrency now describes the
broadcaster's non-blocking publish, the retired unwedge machinery, and the
current Terminate order.)**

### A2. Run-mode clobber on per-turn-borrow turns

`RunOneTurn`'s `PresetRestore` unconditionally resets
`executor_.SetRunMode(params_.run_mode_override.value_or(preset_.run_mode))`
(`src/server/agent/session.cpp:1128-1129`). But a shift+tab run-mode change
(`SubmitRunMode` → `RunOneRunMode`, `session.cpp:650-656`) sets the executor's
mode **without** updating `params_.run_mode_override` or `preset_.run_mode`.
`ApplyPreset` has the same shape (`session.cpp:444`).

Consequence: any subsequent turn carrying a per-turn model/preset override
silently reverts the user's run-mode switch back to the session default. A
headless-scripted borrow after an interactive AllowOnly flip, for example,
quietly goes back to Normal.

Fix direction: record the sticky run mode on the session (the same place
`model_override_` lives) and have `PresetRestore` restore *that*, not the
preset/params slice.

### A3. Subagent-ceiling pattern rules are inert in the common case

`pattern_subject` is computed only when the child's own policy has pattern
rules: `if (!policy_.pattern_overrides.empty())`
(`src/server/tools/tool_executor.cpp:81`). The subagent-ceiling folds consume
that same value: ceiling pattern rules at `tool_executor.cpp:159-161`, ceiling
`out_of_workdir` patterns at `168-179` — contradicting the in-code claim "The
ceiling's own pattern rules ride along" (`tool_executor.cpp:154-156`).

Consequence: a parent preset with `pattern_overrides: {"git *": "ask"}` handed
down as a ceiling **silently never matches** for a child whose own preset has
no pattern rules — which is the overwhelmingly common configuration. The
"child can never be more permissive than its parent" invariant has a hole
exactly where pattern-based guards are used.

Fix direction: compute `pattern_subject` whenever *either* `policy_` or the
ceiling policy carries pattern rules (the perf guard stays — pattern-free
parent+child still pays no filesystem work).

### A4. ForkSession drops persisted model overrides

`SessionDatabase::ForkSession` copies `project_root` and `agent_preset` only
(`src/server/session/session_database.cpp:1452-1461, 1500-1517`). The v8
`model` and v10 `subagent_model` columns are not copied. The proto says an
empty fork model "defaults to the source's preset model"
(`proto/minimal/v1/session_service.proto:767-768`) — so a source session
running under a `/model` override forks onto the preset's default model, not
the model the user chose. Neither the proto comment nor the header documents
this.

This undercuts the fork's own headline use case ("fork now, try another model"
— § ARCHITECTURE *Session traversal*): the fork silently starts from a
different model than the user had pinned, which is also a cold-cache surprise
in the opposite direction from the documented one.

Fix direction: copy the two model columns in the fork transaction (or refuse
to fork a session with a model override, which is worse — copy is clearly
right).

## B. Client-side regressions

The client plugin loader is the server loader's twin — acknowledged as
deliberate duplication ("the FILE FORMAT is the contract both sides
implement") — and it is precisely where the server's fixes have not landed.

### B1. Client loader runs plugin code before the ABI gate

`src/client/plugins/plugin_loader.cpp:163-165` calls the plugin's
`name_fn()`/`version_fn()` (plugin-executed functions) **before** the tag
comparison at `:169`. The server loader fixed exactly this ordering and its
comment says running name/version from a mismatched `.so` "is exactly what the
ABI gate exists to prevent" (`src/server/plugins/plugin_loader.cpp:193-198`).

Severity is low (the results are discarded on mismatch), but it is plugin code
executing against incompatible types — the failure class the gate exists for.

### B2. Client provenance-collision warning scans stale persisted entries

Client `plugin_discovery.cpp:189-193` scans `config.plugins` — which is never
pruned, so entries for deleted files persist forever. The server deliberately
scans `found` instead to avoid warning forever about collisions that no longer
exist (`src/server/plugins/plugin_discovery.cpp:200-217`). The client
reintroduced the false-positive the server fixed.

## C. Provider-layer robustness

### C1. Anthropic reassembler throws on present-but-null stream fields

The Anthropic stream reassembler reads optional fields with nlohmann's
`value()`: `delta.value("text","")`, `value("thinking","")`,
`value("signature","")`, `value("partial_json","")`
(`plugins/server/provider-anthropic/anthropic_stream_reassembler.cpp:111-133`).
`value()` on a **present-but-null** field throws `type_error` — exactly the
failure class the OpenAI side documents and explicitly guards with
`is_null()`/`is_string()` checks everywhere
(`openai_stream_reassembler.cpp:66-71`).

Consequence: a provider/gateway quirk that emits `"text": null` in a keepalive
frame becomes a non-retryable ParseError and kills the turn, where the OpenAI
provider sails through. The two providers were meant to be reviewable mirrors;
this is the divergence that matters.

### C2. OpenAI builder emits only legacy `max_tokens`

`openai_request.cpp:336-338` emits `max_tokens` only. Newer OpenAI models
reject it in favor of `max_completion_tokens`; only an operator hand-writing
`parameters` passthrough can supply the new field, and nothing documents or
auto-translates this. Against a current OpenAI backend, every request fails
until the operator discovers the workaround.

### C3. Anthropic top-level `cache_control` is self-admittedly unverified

`anthropic_request.cpp:265-267` — "NOTE: this field's acceptance by the live
API is unverified from this repo." If a gateway rejects unknown top-level
fields, every Anthropic request through it fails. This is the single
highest-traffic code path in the provider and its one load-bearing field is
unverified.

### C4. AdvisedPermissionChecker's abandoned-thread counter can go negative

The `Decrement` guard fires on *every* task exit, not just abandoned ones
(`src/server/tools/permission_advisor.cpp:111-114`). On the normal path the
counter is decremented from 0 (→ −1); each normal completion offsets a still-
running abandoned +1, weakening the `kMaxAbandonedAdvisors = 8` ceiling under
interleaving (`permission_advisor.cpp:88-93`). The comment's "the pair never
crosses" holds per-thread, not for the shared counter.

## D. Minor

| # | Finding | Where |
|---|---|---|
| D1 | `tools-fs` `write` **does** mkdir (`CreateParents`, `fs_tools.cpp:71-85`) — the "no silent mkdir" claim is true only of the primitive `atomic_write::write_file` (`atomic_write.hpp:30-32`). Same for `edit create` (`fs_tools.cpp:387-390`). Deliberate layering, but any doc keyed on the primitive mis-states tool behavior. | `plugins/server/tools-fs/` |
| D2 | `web_search` has one unchecked narrowing read: `input.at("max_results").get<int>()` after only an `is_number_integer` check — a value outside int range truncates before the clamp. Compare `web_fetch`'s explicit int64 range check (`fetch_tool.cpp:104-106`). **(Fixed 2026-09-12: int64 read + range check, mirroring web_fetch.)** | `plugins/server/tools-web/search_tool.cpp:84-86` |
| D3 | `permission.hpp:99-100` still claims "an exact per_tool_overrides entry **still wins**", but `permission.cpp:39-63` changed exact-vs-pattern to a `most_restrictive` fold; the .cpp comment records the change, the header was not updated. **(Fixed 2026-09-12: header comment now describes the fold.)** | `src/server/tools/` |
| D4 | Hook envelope fields (`hook_point`, `timestamp_unix_ms`, `hook_dispatcher.cpp:151-156`) are on every payload but no point's `filterable` list includes them — filtering on the mechanism's own fields produces a "never match" warning. Possibly deliberate; undocumented either way. **(Resolved 2026-09-12: documented as deliberate in EXTENSIBILITY.md § Hooks — `on:` selects the point; time is not a behavioral selector.)** | `src/server/hooks/` |
| D5 | Default status format references plugin-provided `{cost}`/`{token_detail}` segments (not in `IsKnownStatusSegment`, `status_bar.cpp:71-75`): with client plugins disabled, every startup warns "unknown segment" for the *default* format. | `src/client/` |
| D6 | The proto defines `SetRunModeEvent` as a ServerEvent (`session_service.proto:185`) but the client has no inbound handler branch — `DispatchToTab` falls to the unknown-payload warn. The client relies solely on `SessionMetadataEvent`; latent wire-evolution gap rather than an active bug. **(Resolved 2026-09-12: FALSE POSITIVE — `session_service.proto:185` is in the ClientEvent oneof (client→server); the server announces run-mode changes via `SessionMetadataEvent`, which the client handles. No gap exists.)** | proto + `src/client/state.cpp:1780-1790` |
| D7 | `NameRegistry` dot-in-plugin-name ambiguity is warn-only: `("a.b","c")` and `("a","b.c")` compose the same stable identity, so alias targeting across such plugins can silently mis-resolve (`name_registry.cpp:20-24`, "AMBIGUOUS-by-convention"). **(Fixed 2026-09-12: `Resolve()` now reports real composition ambiguities as `ambiguous_identities`, fatal at the sanity check; a lone dotted plugin stays the Register-time warning.)** | `src/common/plugins/` |
| D8 | `McpClientManager` caller-cancellation detection is a magic-string comparison — `outcome.error == "cancelled"` (`mcp_client_manager.cpp:112`), produced by `WaitSlot` (`mcp_connection.cpp:73`). A rename silently changes whether a cancelled call kills the connection. A typed outcome enum removes the fragility. **(Fixed 2026-09-12: `WaitSlot` returns a typed `WaitResult`; the outcome carries `caller_cancelled`.)** | `src/server/mcp/` |
| D9 | Two definitions of "ambiguous bare permission key" coexist: NameRegistry's unaliased-count logic vs. the sanity check's raw-attribution count (`sanity_check.cpp:745-747`). Both warn; they can disagree in edge cases. | `src/common/plugins/` + `src/server/config/` |
| D10 | `ToolExecutor::granted_keys()` reads `session_grants_` from the spawner path without synchronization; correctness rests on that read also being worker-side (`tool_executor.hpp:53-59, 168-174; cpp:464-468`). Fragile if a spawner ever moves off the worker. | `src/server/tools/` |
| D11 | `ElideLargestToolResult` mutates working history only, not the store — resume regrows the elided bytes and pays one oversized request (documented in-code, `agent_loop.cpp:85-91`). A real semantic gap vs. the checkpoint path, but self-healing. | `src/server/agent/` |
| D12 | Blocking subagent spawn busy-waits at 25 ms on the parent worker rather than waiting on a condition variable (`subagent_manager.cpp:398-410`) — poll-based by design (cancellation must be observed), but it is one wakeup per 25 ms per blocking child. | `src/server/agent/` |
| D13 | Doc/code drift in ARCHITECTURE.md beyond A1: the hook-veto seam is described as living in `ToolExecutor::RunGuarded` — no such symbol exists (it is `PrepareGuarded`, `tool_executor.cpp:148, 258-321`; semantics match); the client's unconditional "5 Hz floor" was replaced by three conditional redraw hints (`app.cpp:1941-1997`). **(Fixed 2026-09-12: ARCHITECTURE.md and docs/EXTENSIBILITY.md now name `PrepareGuarded`; the pump passage describes the three conditional wake hints. Also fixed: the Write/no-mkdir claim (D1's doc half) and the v1 `PendingRequests` grace-timer passage, which now marks that machinery as retired.)** | docs + client/tools |
| D14 | `Get` mode's `CURLE_FILESIZE_EXCEEDED` error reports "response body exceeded 64 MiB" even in Get mode, which caps at 8 MiB; `CURLOPT_MAXFILESIZE_LARGE` is set to the Post cap for all modes (`http_client.cpp:256-257, 314`). The WriteCallback check remains the real authority — but the early-abort threshold is wrong for Get. **(Fixed 2026-09-12: per-mode threshold and message.)** | `src/server/net/` |
| D15 | Anthropic `message_delta` usage merge never lowers figures (`anthropic_stream_reassembler.cpp:170-178`): a server revising a count downward is ignored. Documented rationale exists; the asymmetry is real. | `plugins/server/provider-anthropic/` |

## E. Open verification item

- **TSan suite needs a fresh complete run.** The repo's latest `build-tsan`
  ctest log shows only 24 of 2,198 tests executed before the run ended; the
  `LastTestsFailed.log` there is from Sep 11 and the tests I could locate in
  the current log all report "Test Passed". The `build/` suite is clean
  (2,198/2,198 on Sep 12). Concurrency correctness is this codebase's core
  claim — the TSan tier is the one that proves it, and its status is currently
  unconfirmed.
