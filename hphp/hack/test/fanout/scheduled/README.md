# Scheduled fanout regressions

This harness gives tests deterministic control over when disk changes happen,
including at phase boundaries within a typechecking iteration. It describes
fanout bugs whose reproducer depends on a particular sequence of changes within
or across iterations. Ordinary fanout fixtures and server-loop tests remain in
their existing suites.

Each scenario starts with a full-check baseline, then runs explicit incremental
iterations while retaining server state. The last iteration's diagnostics are
compared with an independent full check of the final files, without an extra
incremental check to repair stale state before comparison.

Tests use the Rust provider and serial execution, with declaration prefetching
disabled. Temporary Hack sources are parsed and typechecked.

See [scheduled_fanout.mli](scheduled_fanout.mli) for the scenario API. Keep
harness self-tests separate from active bug repros under
`regressions/active/`. See [the active bug index](regressions/active/README.md).
Fixed regressions belong directly in `regressions/`.

Tests for fanout bugs that have not been fixed yet can use
`run_expect_bug` to specify the documented diagnostics. If the bug stops
reproducing, the test fails with instructions to remove the wrapper and keep
the correctness check.

Run the harness self-tests with:

```sh
buck test @fbcode//mode/dev-nosan-lg fbcode//hphp/hack/test/fanout/scheduled:harness_tests
```
