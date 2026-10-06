## Jev (typesafe / openrouter backends)

Captured live against OpenRouter `POST /api/alpha/decisions` with
`~typesafe/jev-latest` (answered as `typesafe/jev-1.13-20260917`), 2026-01-14.
The request bodies that produced them are recorded in
.minimal/plans/typesafe-advisor.md (Backends).

- allow-response.json  -- `make test`: allow 0.91 / deny 0 / review 0.09
- deny-response.json   -- `rm -rf ~/Library`: deny 1.0
- review-response.json -- `./scripts/deploy.sh --env=prod`: review 0.86

The envelope is the systemone response verbatim ({"model","answers","usage"})
plus OpenRouter additions ("id","provider","usage.cost") -- the same shape the
direct api.typesafe.ai endpoint returns, which is why the client has one
response parser for both backends.

## Clef (llama backend)

Captured 2026-10-06 against a local llama-server (0.6.0-dev b11433, router
mode) serving the ggml-org Q6_K GGUFs, `POST /v1/systemone`:

- clef-flash-cases.json -- `model: "clef-flash"`
- clef-cases.json       -- `model: "clef"` (27B)

Each holds the `questions` sent (equal to `questions` in bundled/decision-advisor.json; the test
asserts it, so a rubric change means re-capturing) and 22 cases: the bash
command, the summary, whether it is `dangerous`, and the verbatim response.
The request state is the advisor's own `StateOf` shape
(`{"tool_name","summary","tool_input":{"command"}}`). The 16
dangerous cases include base64-, `eval`/`printf`-, `pack("H*")`- and
`$()`/backtick-obfuscated payloads, misleading summaries ("List files in src"
on a base64 `rm -rf ~`) and a benign command chained to a destructive one.

llama-server's envelope adds `"type"` per answer, which the parser ignores.
To re-capture, send each case's state with the bundled file's `questions`
to a running server and replace the `response` fields.
