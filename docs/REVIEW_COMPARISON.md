# Comparative Positioning — `minimal` vs. opencode / Claude Code / Codex

How the `minimal` harness compares, as an **extensible agentic harness**, to the
current generation of agent harnesses. Framed per the project's own terms:
`minimal` is not a coding agent — its coding tools are bundled plugins, the same
kind a third party ships — so "coding-leaning" is noted for the others as a
scope observation, not a defect. Companion document:
[REVIEW_FINDINGS.md](REVIEW_FINDINGS.md) (the verified findings list); the full
per-subsystem review was delivered separately.

Assessment basis: direct code verification of `minimal` at `bf47a2d` (see the
companion document for citations); public, documented behavior of the
comparables as of late 2026. The comparables evolve quickly — treat their rows
as the shape of the platforms, not version-exact feature audits.

## 1. Summary table

| Dimension | minimal | opencode | Claude Code | Codex |
|---|---|---|---|---|
| Extension depth | Native C++ ABI; 12+ seams; transactional registration; fatal collision diagnostics | TS plugin API; moderate | Hooks + MCP + skills; no binary plugins | Closed |
| No-compile tier | `hooks.json`, markdown commands, skills | Strong | Strong | Strong |
| Plugin collision/ordering semantics | Fatal-at-startup, alias-resolvable, attribution-tracked | Best-effort | n/a (no plugin registry) | n/a |
| Sessions & persistence | SQLite; detach/attach N-client; rewind/redo/fork w/ checkpoints | Good | Good | Limited |
| Headless/scripting | First-class; exit-code contract (0/2/3/4/64/69/70); JSONL | Good | Good | Good |
| Multi-provider | Plugin providers + runtime model discovery | Good | Anthropic-centric | OpenAI-centric |
| Ecosystem | None (by design, v1) | Large | Large | n/a |
| Footprint / efficiency | Exceptional (serialized wire bodies, bounded everything, no tokenizer) | Moderate | Moderate | n/a |
| Subagent model | Same-Session children; permission ceilings; admission control | Tabs/tasks | Task tool | Tasks |
| Concurrency architecture | 3 threads/session; gate-then-run; bounded queues; typed rendezvous | Event-loop | Event-loop | Event-loop |
| Non-coding generality | Mechanisms general; shipped examples code-shaped | Code-leaning | Code-leaning | Code |

## 2. Where `minimal` exceeds the comparables

**A real plugin ABI with transactional registration.** opencode's plugin API is
TypeScript function-hooking with no collision story; Claude Code has hooks and
MCP but no binary plugin registry at all; Codex is closed. `minimal` offers
twelve extension points (tools, commands, providers, session observers,
services, permission advisors, hook points, mention resolvers, auth providers,
model catalogs, compaction strategies, project commands), one exclusive slot
(auth), guarded registration that **rolls back on throw and lists what stayed
live** (`src/server/plugins/plugin_loader.cpp:262-274`), and startup-fatal
collision diagnostics naming both claimants (`src/common/plugins/name_registry.cpp:111-141`).
No comparable harness treats "two plugins claim the same name" as a diagnosable
build-time error rather than load-order roulette.

**Hooks contributed by plugins, not fixed by core.** Claude Code's hook points
are a fixed list. `minimal`'s are a typed point registry any plugin can extend
(`minimal_plugin_register_hook_points`) — the bundled `tools-bash.before_command`
point is the proof it works in shipped surface. The guardrails ("hooks only
narrow", fail-open shell contract, declared filterable fields) are enforced by
the core mechanism, not by hook-author discipline.

**Cross-plugin services with seal semantics.** The `ServiceRegistry` pull
channel — typed at the edges by header-only interfaces, named in the middle,
collisions fatal, `Find` returning null as a runtime condition — has no analog
in any comparable. It is what lets `tools-bash` expose shell state to
`/restart-shell` through the exact seam a third party would use
(`bash_state_service.hpp`, `core.restart-shell` via `Find`).

**Seams nothing else opens: compaction strategies and model catalogs.** A
plugin can replace how context is compacted
(`minimal_plugin_register_compaction_strategies`) or add a model-discovery
backend (`register_model_catalogs`). opencode and Claude Code hardcode both.
For a harness meant to run against arbitrary backends (llama.cpp KV-cache
strategies, custom summarizers), these are the two most valuable seams in the
tree and no one else ships them.

**Test fakes as a build target.** `tests/support/` (FakeLlmProvider, deadline
harness, fakes for the one-method interfaces) is exported so plugin authors
test against the same machinery the harness does
(`ARCHITECTURE.md` § Testing). This matters more here than elsewhere because
most functionality *is* plugins.

**Efficiency engineering as a first-class goal.** The request body is
serialized, not materialized (`openai_request.cpp` builds into a plain string);
scrollback is virtualized against SQLite with keyset pagination; every queue,
buffer, and capture in the tree is byte- or count-bounded. The comparables are
Node/event-loop systems where a 1M-token history is a RAM problem; `minimal`
handles it as a paging problem. Nothing in this class makes the 512k–1M token
claim plausibly.

**Honest security modeling.** "The permission gate is the boundary, not an
allowlist" — with the allowlist rejection argued rather than asserted, consent
scoping that names resolved program paths (`bash_tool.cpp:131-174`), SSRF
defense that resolves and *pins* addresses against DNS rebinding
(`url_policy.cpp:167-208`), and wire-level grant filtering against what was
actually disclosed (`wire_permission_checker.cpp:61-70`). The comparables'
permission models are less precisely specified; Claude Code's allowlist
machinery is exactly the pattern `minimal` argues against (and its own docs
concede the failure modes `minimal` refuses to ship).

## 3. Where the comparables are ahead

**Ecosystem.** opencode and Claude Code have marketplaces, community plugins,
and installation flows. `minimal` v1 has local-directory discovery, provenance
keying, and a disclosed "no remote plugin installation or registry" policy
(`ARCHITECTURE.md` § Distribution). This is a deliberate v1 scope decision, but
it is the single largest practical gap: a harness lives or dies by what people
build on it.

**No-compile authoring depth.** `minimal`'s no-compile tier (hooks.json,
markdown commands, skills, agent presets) is competent but shell-shaped.
opencode/Claude Code let a user go from idea to working extension without a
C++ toolchain; in `minimal`, anything beyond a hook or a prompt-command means
compiling a `.so` against the tree. The source-compatibility model (ABI tag as
a "you forgot to rebuild" guard) is honest but means **no out-of-tree binary
distribution of plugins at all**.

**Onboarding cost.** A plugin author for opencode writes TypeScript they
already know. A plugin author for `minimal` reads a 1,800-line architecture
document, links a shared core library, and inherits three-threads-per-session
concurrency obligations. The comment discipline in the tree (every guard
citing the incident it prevents) mitigates this substantially — but the entry
price is real and the comparables' is near zero.

**Client-side polish breadth.** Claude Code and opencode have years of
terminal-UX iteration. `minimal`'s TUI is architecturally cleaner (derived tab
states, bounded scrollback, snapshot pumping) but younger; the review found
the client is also the least-tested half and the side where the server's
loader fixes haven't landed (companion doc §B).

## 4. Non-coding generality — the stated goal, assessed

The mechanisms are genuinely coding-agnostic:

- **Signals** (`src/server/agent/signal_router.*`) are a general external-event
  ingress — CI completions, bug filings, any MCP stdio channel — with
  subscription gating, provenance framing, and cross-session messaging. No
  comparable ships anything like this; Claude Code's closest analog is its
  notification hooks, which are push-out, not ingress-in.
- **Skills, presets, prompts** are plain config files, indifferent to domain.
- **The tool set is a plugin choice**: disable `tools-fs`/`tools-bash`, ship
  different tools, keep the entire permission/transport/session machinery.
- **Compaction, token accounting, sessions** know nothing about code.

What is *not* yet general is the **worked-example layer**. The bundled plugins
are all code-shaped (fs/bash/search/web/python). The designed-but-deferred
`llamacpp-kv` plugin is the one non-coding-adjacent worked example, and it
isn't shipped. A third party cannot look at the tree and see "here is how I
build a writing harness" or "here is how I build a data-analysis harness" —
they must infer it from the mechanisms. This is the gap between the
architecture's claim and the shipped product's demonstration of it, and it is
the cheapest gap to close: one or two canonical non-coding plugin pairs
(server tool + client view) would prove the generality the design asserts.

## 5. Bottom line

As a **harness** — the thing the repo is named after — `minimal` is the most
rigorously extensible system in its class on paper and in code: more extension
points, harder guarantees, better failure-mode documentation, and a
concurrency architecture the comparables don't attempt. It is the least
usable *ecosystem*, by declared design, and its no-compile tier doesn't yet
compensate for the C++ entry price. Its two structural risks are:

1. **Out-of-tree plugin authorship** — source compatibility plus a 62k-LOC
   codebase means the plugin story is currently "fork the tree," which caps
   the community the architecture was built to serve; and
2. **Doc/code drift at the edges** — the mechanism documentation is
   exceptional but already trails the code in the corners (dead EventQueue
   machinery described as live, `RunGuarded`, the 5 Hz floor, the header/impl
   split on permission precedence). For a project whose documentation *is*
   part of its extensibility contract, drift here is more costly than usual.

Both are addressable; neither is architectural.
