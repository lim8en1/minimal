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
