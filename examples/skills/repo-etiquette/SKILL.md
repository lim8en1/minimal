---
name: repo-etiquette
description: Project conventions for commit messages, naming, and where things live.
---

How we do things in this repository. Applies before any file is written here.

**Commit messages**: imperative subject line under 72 characters, no period,
body only when the "why" is not obvious from the diff. Scope prefix matches
the top-level directory touched (`feat(agent):`, `fix(tui):`).

**Names**: files and functions in snake_case; test files end in `_test.cpp`
and live in `tests/`; a test's tag string matches the subsystem directory.

**Layout**: new server subsystems go in `src/server/<name>/` with headers
under `include/minimal/<name>/`; anything reusable by plugins belongs in
server_core, not in a plugin.

**Verification**: `make test` before declaring anything done; sanitizers
(`make asan`) for anything touching ownership or threading.

**Never**: edit generated files under `build*/`, commit anything under
`.opencode/worktrees/`, or leave a TODO without an owner's initials.
