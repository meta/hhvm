# Scheduled fanout tests

This harness gives tests deterministic control over when disk changes happen,
including at phase boundaries within a typechecking iteration. It describes
fanout bugs whose reproducer depends on a particular sequence of changes within
or across iterations. Ordinary fanout fixtures and server-loop tests remain in
their existing suites.

Each scenario establishes a baseline through a full check or saved-state load,
then runs explicit incremental iterations while retaining server state. The
last iteration's diagnostics are
compared with an independent full check of the final files, without an extra
incremental check to repair stale state before comparison.

Tests use the Rust provider and serial execution, with declaration prefetching
disabled. Temporary Hack sources are parsed and typechecked.
`Evict_decls` actions remove selected current non-class declarations and clear
local caches, so tests can force missing old declarations during redeclaration.
This is stronger than real shared-memory eviction, which can leave local caches
warm; it models observing the evicted state later with cold local caches.

See [scheduled_fanout.mli](scheduled_fanout.mli) for the scenario API. Keep
harness self-tests separate from active bug repros under
`tests/active/`. See [the active bug index](tests/active/README.md).
Other tests belong directly in `tests/`.

Remote old-declaration fixtures are files at their original repository-relative
paths, provided through `remote_old_decls`. Only files selected by the normal
saved-state lookup are loaded from the fixture folder, which lives outside the
checked repository. `tests/remote_old_decls.ml` covers the class regression
formerly in the Python integration suite.

Tests for fanout bugs that have not been fixed yet can use
`run_expect_bug` to specify the expected diagnostic difference or internal
typechecker error. If the bug stops reproducing, the test fails with instructions
to remove the wrapper and keep the correctness check.

Run the harness self-tests with:

```sh
buck test @fbcode//mode/dev-nosan-lg fbcode//hphp/hack/test/fanout/scheduled:harness_tests
```
