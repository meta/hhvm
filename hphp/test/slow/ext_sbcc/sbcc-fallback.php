<?hh

/**
 * Test: Nonexistent Eval.SBCCPath → graceful fallback.
 *
 * Eval.SBCCPath points to a file that doesn't exist. HHVM should not crash
 * and init_errors should be incremented.
 *
 * This test uses .config to set Eval.SBCCPath directly (rather than sbcc_files)
 * because the scenario under test is specifically a missing file path — there
 * is no artifact to materialize, and the runner should not attempt to build one.
 */

<<__EntryPoint>>
function main(): void {
  $global = HH\SBCC\get_global_stats();
  echo "init_errors:".$global['init_errors']."\n";
  echo "survived\n";
}
