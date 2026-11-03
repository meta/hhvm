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
| Duplicate-property initializer dependencies | duplicate_trait_property_initializer.ml | Changes to property initializers can affect duplicate-property warnings in classes using those properties, but declaration comparison does not detect those changes. |
| Name-suggestion dependencies | unbound_name_case_suggestion.ml | Diagnostics depend on case-insensitive name lookup, but the dependency graph does not track changes to that lookup. |
| Duplicate member declarations | duplicate_member_declarations.ml | Declaration comparison uses the first duplicate member, while folding takes parts of the effective declaration from the last duplicate. |
| Naming-winner changes | case_variant_duplicate_winner_flip.ml | A case-insensitive naming conflict can change which definition is visible without changing the exact-name declarations used for fanout comparison. |
