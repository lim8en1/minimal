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
