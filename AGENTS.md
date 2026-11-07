# Notes for coding agents

minimal is an extensible, plugin-based, general-purpose agent harness in
C++20: a gRPC server (`src/server`), an FTXUI terminal client (`src/client`),
shared code (`src/common`) and plugins (`plugins/server`, `plugins/client`).
Read [ARCHITECTURE.md](ARCHITECTURE.md) before structural changes and
[CONTRIBUTING.md](CONTRIBUTING.md) for the contribution rules.

## Build and test

```sh
make                 # configure (first run) + build; no test binaries
make test            # build test binaries + run ctest
cmake --build --preset default --target minimal_tests   # rebuild tests only
build/tests/minimal_tests "[tools_bash]"                 # one Catch2 tag
```

The test binaries are `EXCLUDE_FROM_ALL`: a plain build leaves
`build/tests/minimal_tests` stale. Rebuild it explicitly before running tests
you changed.

## Rules the build enforces

- **Plugins see only the SDK.** A plugin may include its own headers and
  those in `scripts/sdk_headers.txt` (server) or
  `scripts/client_sdk_headers.txt` (client). Needing anything else means a
  missing extension point -- add it deliberately, don't reach around it.
- **Server layering.** `src/server/<module>` may include only modules in its
  CMakeLists `DEPS` (`scripts/check_module_deps.py`).
- **No bare policy literals.** Limits, timeouts and sizes are named constants
  in the module's `policy.hpp` (`policy_constants` test, needs clang-tidy).

## Conventions

- New capability goes in a plugin unless it is a mechanism the core must own.
  Each plugin owns its config schema (`plugins.d/<name>.json`), its
  permission policy and its UI.
- No speculative extension points: add one together with its first real
  consumer.
- While in beta: remove obsolete code paths outright instead of keeping
  compatibility shims.
- Match the surrounding code's naming, comment density and idioms; comments
  say why. New behaviour comes with tests under `tests/`.
- User-facing config, commands or plugin surface changes update
  `docs/CONFIGURATION.md` / `docs/PLUGINS.md` in the same change.
- Commits: `area: what changed`, a body that says why, and a DCO sign-off
  (`git commit -s`).
