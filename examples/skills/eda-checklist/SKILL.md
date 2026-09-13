---
name: eda-checklist
description: Exploratory data analysis checklist -- the checks to run before trusting a dataset.
---

Run this checklist, in order, before any analysis over an unfamiliar dataset.
Report the outcome of each step in one line, even when it is "clean".

1. **Identity**: how many rows and columns; is each row one observation, and
   of what? State it in plain words.
2. **Missingness**: per-column null counts; distinguish "empty because it
   does not apply" from "empty because collection failed".
3. **Types**: does each column's dtype match what its name claims? Dates
   parsed as strings and IDs as numbers are the classic silent rot.
4. **Ranges**: min/max/percentiles for numeric columns; flag physically
   impossible values (negative ages, percentages over 100).
5. **Duplicates**: exact-duplicate rows; near-duplicates on the natural key.
6. **Balance**: category frequencies; a class at 0.01% changes which methods
   are even admissible.
7. **Leakage**: any column the model would not have at prediction time --
   outcomes, post-hoc timestamps, aggregates of the target.

Findings feed the analysis plan; a step that fails is information, not an
obstacle -- say what you would do about it and ask if the obvious fix
(dropping, imputing, re-querying) needs a human decision.
