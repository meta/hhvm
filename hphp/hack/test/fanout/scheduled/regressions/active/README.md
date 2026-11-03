# Active fanout bugs

These tests document fanout bugs that have not been fixed yet. Each scenario
compares incremental diagnostics with a fresh check of the final files.
`run_expect_bug` accepts the documented difference; when the bug stops
reproducing, the test fails with instructions to keep the scenario as a normal
correctness check.

Each file corresponds to one familly/underlying bug, with one or more repros.

| Bug family | Repro tests | Underlying bug |
| --- | --- | --- |
| Intermediate declaration states | missing_class_after_restore.ml | Fanout compares old and new declarations without accounting for intermediate states observed by dependent checks. |
| Unprocessed declarations used as old | current_decl_used_as_old.ml | Lazy declaration reads can cache unprocessed edits, which declaration comparison then uses as its old baseline. |
| Private member inheritance effects | private_member_addition.ml | Private member additions and removals outside traits are classified as not affecting descendants, even when they change inherited members. |
| Required-class member dependencies | require_class_member_dependency.ml | Eager member lookup through a trait's required class omits dependencies needed for member-level fanout. |
