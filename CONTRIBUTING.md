# Contributing

Issues and pull requests are welcome. For anything larger than a small fix,
open an issue first so the approach can be agreed before you write it.
Security problems go through private reporting instead -- see
[SECURITY.md](SECURITY.md).

## Before you start

- [ARCHITECTURE.md](ARCHITECTURE.md) explains the core and why it is small.
  Most new capability belongs in a plugin, not in `src/`
  ([docs/EXTENSIBILITY.md](docs/EXTENSIBILITY.md),
  [docs/PLUGINS.md](docs/PLUGINS.md); `scripts/new-plugin.sh` scaffolds one).
- Plugins build against the SDK headers only (`scripts/sdk_headers.txt`,
  `scripts/client_sdk_headers.txt`); the `plugin_conformance` test enforces it.

## Build and test

See [BUILD.md](BUILD.md) for prerequisites. Then:

```sh
make test          # build with tests + ctest
make asan-test     # AddressSanitizer
make tsan-test     # ThreadSanitizer, for anything touching threads
```

A pull request should pass `make test` on your platform. The suite includes
gates beyond unit tests: `policy_constants` (no bare numeric literals for
limits and timeouts -- name them in a `policy.hpp`; needs clang-tidy) and
`plugin_conformance`. CI builds Linux on every pull request.

## Style

- Match the surrounding code: its naming, comment density and idioms.
  `.clang-tidy` holds the lint configuration.
- Comments explain why, not what.
- New behaviour comes with tests (Catch2, under `tests/`).
- Commit messages: `area: what changed` as the subject (e.g.
  `tools-bash: ...`, `client: ...`), and a body that says why.

## Sign your commits (DCO)

Contributions are accepted under the project's
[Apache 2.0 license](LICENSE). Each commit must carry a sign-off certifying
the [Developer Certificate of Origin](https://developercertificate.org/):
that you wrote the change, or otherwise have the right to submit it under
this license.

```sh
git commit -s      # adds "Signed-off-by: Your Name <you@example.com>"
```

Use your real name or a consistent public identity, and an email you can be
reached at. To sign off commits already on your branch:
`git rebase --signoff main`.
