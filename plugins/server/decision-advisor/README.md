# decision-advisor

Auto-mode as a permission advisor (docs/EXTENSIBILITY.md §3) — a decision
model (TypeSafe's Jev, or Cloudflare's Clef on a local llama-server) answers the
tool-permission prompts the substrate resolved to "ask a human"; anything it is
not confident about falls through to you in the usual permission modal.

## What it adds

- **PermissionAdvisor**: every `Ask`-gated tool call is evaluated
  against a System One API endpoint. A confident answer resolves the prompt
  without you; the modal appears exactly as before for everything else.
- **Auto-allow and auto-deny**, gated asymmetrically: deny requires more
  confidence than allow, because a wrong auto-allow hands the model a
  capability while a wrong auto-deny merely produces a refusal the model can
  act on. A denial carries a message ("auto-denied by decision-advisor … ask
  the user before retrying") so the model knows what happened and why.
- All backends below speak the **same request/response shape** (verified
  live): the body is `{"state", "model", "questions"}` and the answer is
  `{"model", "answers", "usage"}`. Only the endpoint, key, model id and
  default thresholds differ.

## Ships disabled

`decision-advisor` is built `DISABLED_BY_DEFAULT`, like `session-titler`: it
sends tool input to an external endpoint and auto-answers
permission prompts. Enabling it is two deliberate edits:

```json
// plugins.json: the plugin's entry
{ "file": "decision-advisor.so", "enabled": true }
```

```json
// plugins.d/decision-advisor.json
{ "api_key": "secret:typesafe" }
```

```json
// permissions.json (merge into the existing object)
{ "advisor": "decision-advisor" }
```

Optional: `"advisor_inherit": true` also lets it answer prompts inside
**subagent** sessions (default: off — children keep their human even when the
parent is advised). An unset `env:` variable or a `secret:` not stored yet
is a startup warning, and every advisor call abstains naming the fix —
never a silent 401.

## Backends

The profiles shipped in `decision-advisor.json` (see Configuration):

| `backend` | Endpoint | Model | Key |
|---|---|---|---|
| `typesafe` (the file's default) | `https://api.typesafe.ai/v1/systemone` | `jev-latest` | `env:TYPESAFE_API_KEY` |
| `openrouter` | `https://openrouter.ai/api/alpha/decisions` | `~typesafe/jev-latest` | `env:OPENROUTER_API_KEY` |
| `llama` | `http://localhost:8080/v1/systemone` | `clef-flash` | none |

OpenRouter serves Jev through its `/api/alpha/decisions` surface — the
chat/completions endpoint refuses it ("a decisions model"). Same body, same
envelope; only the model id namespace differs. The response's `model` field
reports the versioned id that actually answered, which lands in the log line.

### Running Clef locally (`llama`)

llama.cpp's server answers `POST /v1/systemone` in the same shape (llama.cpp
PR #29831 added Cloudflare's [Clef](https://huggingface.co/Cloudflare/clef)
decision model, text-only so far). Nothing leaves the machine:

```sh
llama-server -hf ggml-org/Clef-Flash-GGUF          # or ggml-org/Clef-GGUF (27B)
```

```json
// plugins.d/decision-advisor.json
{ "backend": "llama" }
```

- **Use the ggml-org GGUFs.** A conversion that lost Clef's decision head
  loads as a plain `qwen35` model, and the server answers every call with
  501 "This model is not a decision model" — which the advisor's warn line
  quotes.
- **Router mode** (`--models-preset`): the body's `model` picks the model,
  so set `model` to the preset's alias (the profile ships `clef-flash`). The
  first call loads it (4–7 s measured), which is why the profile's `budget_ms`
  is 5000; one cold-start prompt may still fall through to you.
- **A key** for a server started with `--api-key`: `/secret set llama <key>`
  and `"api_key": "secret:llama"` (any reference works, docs/CONFIGURATION.md
  § Secrets). Unset, no `Authorization` header is sent.
- **The profile's thresholds are lower** — `min_probability_allow` 0.45,
  `min_probability_deny` 0.50, `min_margin` 0.20 — because Clef's probabilities
  are far flatter than Jev's: on 22 captured bash calls
  (tests/fixtures/decision-advisor/) benign calls topped out at p(allow)
  0.37–0.57 and no dangerous call (base64/`eval`/`pack`/`$()`-obfuscated
  payloads included) rose above p(allow) 0.32. Under Jev's 0.75/0.85 floors
  the advisor would abstain on everything. Re-tune when you change models or
  `questions`: the llama docs note the probabilities are temperature-scaled,
  not calibrated for your data.
- Clef-Flash answers in ~0.25 s warm; full Clef in ~1 s, and is *less*
  decisive on deny (more calls fall through to you), so Flash is the default.
- The whole prompt must fit in the server's `--ubatch-size`.

## Configuration

Every value comes from config — the code has no defaults of its own. Three
layers, later wins:

1. **`<config dir>/decision-advisor.json`**, seeded from
   [bundled/decision-advisor.json](../../../bundled/decision-advisor.json) at
   install and yours from then on (never overwritten): the shared knobs
   (`retry`, `questions`, `extra_questions`), the default `backend`, and one
   profile per backend under `backends`.
2. **`backends.<backend>`** in that file: the profile's `endpoint`, `model`,
   `api_key`, timeouts and thresholds. Add your own profile there (another
   self-hosted endpoint, a second model) and select it by name.
3. **The plugin's own file**, `plugins.d/decision-advisor.json`: any key below,
   plus `backend` — e.g. `{ "backend": "llama", "endpoint":
   "http://localhost:8000/v1/systemone" }`.

`retry` merges field by field; every other key, `questions` included,
replaces wholesale. Strict throughout: an unknown key at any layer, an unknown
backend, a wrong-typed or out-of-range value, a value no layer sets, or a
missing `decision-advisor.json` keeps the plugin from loading, naming the
field. Restore a deleted file from `bundled/`.

| Key | Shipped value | Meaning |
| --- | --- | --- |
| `backend` | `typesafe` | Which profile under `backends` to use (`typesafe`, `openrouter`, `llama`, or one you add). |
| `api_key` | per profile (`env:TYPESAFE_API_KEY` / `env:OPENROUTER_API_KEY` / `""`) | A key reference (docs/CONFIGURATION.md § Secrets). Read per call: an unset `env:` variable or a `secret:` not stored yet is a startup warning, and each call abstains naming the fix. Empty sends no `Authorization` header. |
| `endpoint` | per profile | The System One URL. |
| `model` | per profile | The model id sent in the body. Pin the versioned id (`jev-1.13.0` / `typesafe/jev-1.13`) once you tune thresholds — aliases move. |
| `request_timeout_ms` | `2000` (`llama`: `5000`) | Per-attempt transfer cap. The healthy call is 0.1–0.5 s. Must not exceed `budget_ms`. |
| `budget_ms` | `2000` (`llama`: `5000`) | Total wall clock for one advisor call: attempts **and** backoff sleeps. When it runs out the advisor abstains. Keep below the host's `advisor_timeout_ms` (default 30 s), which stays the outer ceiling. |
| `retry.max_retries` | `2` | Retries after the first attempt, on 408/429/5xx, timeouts and connection failures (the TypeSafe SDK's retryable set). 401/422, 501 (llama-server: not a decision model) and malformed bodies never retry — they recur, and the warn line is how you find out. |
| `retry.backoff_initial_ms` / `retry.backoff_max_ms` | `100` / `2000` | Exponential backoff, doubling per attempt. |
| `retry.jitter` | `0.25` | Fraction of each delay subtracted at random (0 = deterministic; the tests rely on that). |
| `retry.retry_after_cap_ms` | `1000` | A server `Retry-After` is honored up to this — never past the budget. |
| `min_probability_allow` | `0.75` (`llama`: `0.45`) | Census floor: the chosen option's probability must clear it. |
| `min_probability_deny` | `0.85` (`llama`: `0.50`) | Deny's floor — stricter than allow's, deliberately. |
| `min_margin` | `0.25` (`llama`: `0.20`) | Majority margin: the top option must beat the runner-up by this much. Both gates, because TypeSafe's own cookbook measured label flips exactly where top probabilities were close, and a floor alone cannot see a near-tie. |
| `deny_message` | "the call looks destructive or out of scope -- ask the user before retrying" | What a denied model reads, after `auto-denied by decision-advisor (model M): p(deny)=P; `. |
| `questions` | the rubric in the file | The questions map sent verbatim. The answer is read from its `"action"` key: `"type": "choice"` with `criteria` for exactly `allow`, `deny` and `review` (checked at load). Changing the wording changes the probabilities: re-tune the thresholds after. |
| `extra_questions` | `{}` | Merged into the same request (one round trip, the speculative fan-out pattern); answers are logged, never acted on. |

### Fixed in code

What stays in code is protocol or representation, not tuning:

- **The System One wire format**: `state`/`model`/`questions` in the body,
  `answers.action.choice`/`probabilities` out, `Authorization: Bearer`,
  `Retry-After`/`retry-after-ms`. The option set `{allow, deny, review}` is
  what the decision rule acts on; the rubric must match it.
- **Which failures retry**: 408, 429 and 5xx except 501; connection failures
  and timeouts. HTTP semantics (TypeSafe's SDK policy), plus llama-server's
  501 for a model that no retry fixes.
- **The 25 ms cancel poll** during backoff sleeps: the longest a cancel waits
  to be noticed, below what anyone waiting on a prompt perceives.
- **200 characters** of a server's error quoted in the warn line, so it stays
  one line.
- **Range bounds**: probabilities and `jitter` in [0, 1]; milliseconds at
  least 1; nothing above what an `int` holds. The budget, not a cap, bounds
  a call.

## The decision rule

From the answer's `probabilities` (the raw surface — the derived `confidence`
is logged but not gated on):

1. `review` chosen → abstain.
2. Top option below its floor (`min_probability_*`) → abstain.
3. Top option not ahead of the runner-up by `min_margin` → abstain.
4. `allow` → allow this one call. No session grant is minted, so the next
   gated call is judged again.
5. `deny` → deny with the message above.

Abstention is the common denominator: HTTP trouble, a spent budget, a cancel,
an unparseable answer and an honest "review" all cost the same thing — one
human prompt. A judge that cannot answer never blocks a turn.

## Failure posture

- **Fail open, loudly.** The substrate (mode lattice, ceilings, run modes,
  grant filtering) is untouched; the advisor only narrows what reaches you.
  Under `allow_only` headless runs the advisor is never consulted at all — the
  Ask is denied before the advisor exists.
- **Bounded.** One call costs at most `budget_ms` plus the host's own advisor
  budget machinery (which detaches a wedged advisor and prompts you anyway).
- **Every decision logged**, one line: chosen option, p1/p2, answering model,
  attempts, total milliseconds. The `total_ms` of first-attempt successes is
  the number to watch — it should sit in the 100–500 ms range.

## What this is not

Not a safety boundary. Jev evaluates model-controlled text: a crafted
`tool_input` can read harmless and be anything the tool will actually do (for
bash, the disclosure itself is only a lower bound). "Auto-allowed" means *Jev
said yes above your thresholds* — an assistant for prompt fatigue, not a
sandbox. The permission model — modes, ceilings, grant filtering — remains the
boundary, exactly as with a human answering.
