# Security Policy

## Reporting a vulnerability

Please do not open a public issue for security problems. Use GitHub's private
reporting instead: the **Security** tab of this repository, then
**Report a vulnerability**.

Include the affected version or commit, reproduction steps, and the impact you
expect. This is a single-maintainer project; expect an initial response within
about a week.

## Scope

`minimal` runs a local, single-user server that executes tools (including shell
commands) on the user's behalf, gated by `permissions.json`. In scope:
permission or confinement bypasses, sandbox/path-escape bugs, credential
leakage (config, logs, session storage), and memory-safety issues in the server,
client, or bundled plugins. Out of scope: behavior that requires the user to
disable permissions (`--dangerously-bypass`) or to install an untrusted plugin.

## Repository content

A cloned repository can ship its own agents, skills and commands. By default
(`project_local.mode: "trusted"` in `settings.json`) these are **not loaded**
until the user runs `/trust-directory` for that repository; project-local
presets that do load can only narrow permissions, never widen them. Setting the
mode to `"all"` removes this gate and is the user's explicit choice. See
`docs/CONFIGURATION.md`, `project_local.*`.
