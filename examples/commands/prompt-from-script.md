---
description: Compose a review prompt from the repository's own state
sources:
  composed:
    sh: "./scripts/compose-review-prompt.sh"
    output_cap: {max: 20000, on_exceed: confirm}
---
