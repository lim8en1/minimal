# typesafe-advisor

Auto-mode as a permission advisor (docs/EXTENSIBILITY.md §3) — Jev answers the
tool-permission prompts the substrate resolved to "ask a human"; anything it is
not confident about falls through to you in the usual permission modal.

## What it adds

- **PermissionAdvisor**: every `Ask`-gated tool call is evaluated
  against TypeSafe's System One API. A confident answer resolves the prompt
  without you; the modal appears exactly as before for everything else.
- **Auto-allow and auto-deny**, gated asymmetrically: deny requires more
  confidence than allow, because a wrong auto-allow hands the model a
  capability while a wrong auto-deny merely produces a refusal the model can
  act on. A denial carries a message ("auto-denied by typesafe-advisor … ask
  the user before retrying") so the model knows what happened and why.
- Both backends below speak the **same request/response shape** (verified
  live): the body is `{"state", "model", "questions"}` and the answer is
  `{"model", "answers", "usage"}`. Only the endpoint, key variable and model id
  differ.

## Ships disabled

`typesafe-advisor` is in the `DefaultsDisabled` list, like `session-titler`: it
sends tool input to an external endpoint and auto-answers
permission prompts. Enabling it is two deliberate edits:

```json
// plugins.json
{
  "file": "typesafe-advisor.so",
  "enabled": true,
  "config": { "api_key_env": "TYPESAFE_API_KEY" }
}
```

```json
// permissions.json (merge into the existing object)
{ "advisor": "typesafe-advisor" }
```

Optional: `"advisor_inherit": true` also lets it answer prompts inside
**subagent** sessions (default: off — children keep their human even when the
parent is advised). The env var must exist at server startup; an unset key
loads the plugin degraded with a clear error, never a silent 401 on the first
gated call.

## Backends

| `backend` | Endpoint | Default model | Key env |
|---|---|---|---|
| `typesafe` (default) | `https://api.typesafe.ai/v1/systemone` | `jev-latest` | `TYPESAFE_API_KEY` |
| `openrouter` | `https://openrouter.ai/api/alpha/decisions` | `~typesafe/jev-latest` | `OPENROUTER_API_KEY` |

OpenRouter serves Jev through its `/api/alpha/decisions` surface — the
chat/completions endpoint refuses it ("a decisions model"). Same body, same
envelope; only the model id namespace differs. The response's `model` field
reports the versioned id that actually answered, which lands in the log line.

## Configuration

The plugin's own `config` object in its `plugins.json` entry. Every field is
optional; wrong-typed values degrade to the default.

```json
{
  "file": "typesafe-advisor.so",
  "enabled": true,
  "config": {
    "backend": "typesafe",
    "api_key_env": "TYPESAFE_API_KEY",
    "endpoint": "",
    "model": "",
    "request_timeout_ms": 2000,
    "budget_ms": 2000,
    "retry": {
      "max_retries": 2,
      "backoff_initial_ms": 100,
      "backoff_max_ms": 2000,
      "jitter": 0.25,
      "retry_after_cap_ms": 1000
    },
    "min_probability_allow": 0.75,
    "min_probability_deny": 0.85,
    "min_margin": 0.25,
    "questions": {},
    "extra_questions": {}
  }
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `backend` | `typesafe` | Which endpoint family to call (`typesafe` or `openrouter`). |
| `api_key_env` | per backend | Env var NAME the key is read from, once, at registration. |
| `endpoint` | per backend | Override the URL (a self-hosted System One-compatible endpoint). |
| `model` | per backend | The model id. Pin the versioned id (`jev-1.13.0` / `typesafe/jev-1.13`) once you tune thresholds — aliases move. |
| `request_timeout_ms` | `2000` | Per-attempt transfer cap. The healthy call is 0.1–0.5 s. Clamped to `budget_ms`. |
| `budget_ms` | `2000` | Total wall clock for one advisor call: attempts **and** backoff sleeps. When it runs out the advisor abstains. Keep below the host's `advisor_timeout_ms` (default 30 s), which stays the outer ceiling. |
| `retry.max_retries` | `2` | Retries after the first attempt, on 408/429/5xx, timeouts and connection failures (the TypeSafe SDK's retryable set). 401/422 and malformed bodies never retry — they recur, and the warn line is how you find out. |
| `retry.backoff_initial_ms` / `retry.backoff_max_ms` | `100` / `2000` | Exponential backoff, doubling per attempt. |
| `retry.jitter` | `0.25` | Fraction of each delay subtracted at random (0 = deterministic; the tests rely on that). |
| `retry.retry_after_cap_ms` | `1000` | A server `Retry-After` is honored up to this — never past the budget. |
| `min_probability_allow` | `0.75` | Census floor: the chosen option's probability must clear it. |
| `min_probability_deny` | `0.85` | Deny's floor — stricter than allow's, deliberately. |
| `min_margin` | `0.25` | Majority margin: the top option must beat the runner-up by this much. Both gates, because TypeSafe's own cookbook measured label flips exactly where top probabilities were close, and a floor alone cannot see a near-tie. |
| `questions` | built-in | The questions map sent verbatim; override wholesale to change the rubric. The answer is still read from the `"action"` key and must be a Choice over `{allow, deny, review}`. |
| `extra_questions` | none | Merged into the same request (one round trip, the speculative fan-out pattern); answers are logged, never acted on. |

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
