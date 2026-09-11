---
description: Review the working tree's uncommitted changes
args: [focus]
optional: [count]
sources:
  branch:
    run: ["/usr/bin/git", "branch", "--show-current"]
  changes:
    run: ["/usr/bin/git", "diff", "--stat", "HEAD~{count}"]
    output_cap: {max: 4000, on_exceed: fail}
---
The current branch is {branch}.
Review the changes with a focus on {focus}:

{changes}
