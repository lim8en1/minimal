---
description: Draft release notes from recent commits (pass the version)
---
Draft release notes for $1.

Commits since the last tag:
!`git log --oneline $(git describe --tags --abbrev=0)..HEAD | head -n 40`
