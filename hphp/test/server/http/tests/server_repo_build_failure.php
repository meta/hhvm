<?hh

// A failed server repo build fails fast instead of producing a launch command.
function repo_build_failure_throws(string $repo_args): bool {
  try {
    getServerCmd(
      20000, 20001, false, __DIR__.'/..', __DIR__.'/../server_root',
      '', null, $repo_args,
    );
  } catch (Exception $e) {
    return strpos($e->getMessage(), 'Repo build failed') === 0;
  }
  return false;
}

// The compiler joins its file cache after finalizing hhvm.hhbc, so a missing
// static directory fails the build after the repo has been written.
function late_repo_build_failure(): dict<string, bool> {
  $attempt = repo_build_dir(ServerUtilServerTests::$next_instance_id);
  $cache = $attempt.'.filecache';
  $throws = repo_build_failure_throws(
    '--inputs=repo_mode.php --file-cache '.escapeshellarg($cache).
    ' --cdir server_repo_build_failure_missing_dir',
  );
  $log = file_get_contents(ServerUtilServerTests::$LOG_ROOT.'_test'.
    ServerUtilServerTests::test_run_id().'.log');
  $result = dict[
    'throws' => $throws,
    'compiled' => $log !== false &&
      strpos($log, 'std::exception: FileUtil::find(): unable to open directory ') !== false &&
      strpos($log, 'server_repo_build_failure_missing_dir/') !== false,
    'cleaned' => !file_exists($attempt.'/hhvm.hhbc') &&
      !file_exists($attempt.'/hhvm.hhbc.part'),
  ];
  // File-cache output is separate from the harness-owned repository output.
  try_unlink($cache);
  try_unlink($cache.'.part');
  return $result;
}

function repo_cleanup_preserves_diagnostics(): dict<string, bool> {
  $dir = ServerUtilServerTests::$LOG_ROOT.'_cleanup'.
    ServerUtilServerTests::test_run_id();
  mkdir($dir);
  file_put_contents($dir.'/hhvm.hhbc', 'repo');
  file_put_contents($dir.'/hhvm.hhbc.part', 'partial');
  remove_repo_build($dir);
  $empty_removed = !is_dir($dir);
  remove_repo_build($dir);

  mkdir($dir);
  file_put_contents($dir.'/hhvm.hhbc', 'repo');
  file_put_contents($dir.'/diagnostic.txt', 'keep');
  remove_repo_build($dir);
  $diagnostic_kept = file_get_contents($dir.'/diagnostic.txt') === 'keep' &&
    !file_exists($dir.'/hhvm.hhbc');
  try_unlink($dir.'/diagnostic.txt');
  remove_repo_build($dir);
  return dict['empty_removed' => $empty_removed, 'diagnostic_kept' => $diagnostic_kept];
}

<<__EntryPoint>>
function main(): void {
  require_once('test_base.inc');
  init();
  $early = '--not-a-repo-compiler-option';
  $throws = repo_build_failure_throws($early);
  // The cached build entry must be dropped, so the same build runs again.
  $rebuilt = repo_build_failure_throws($early);
  $late = late_repo_build_failure();
  $cleanup = repo_cleanup_preserves_diagnostics();
  echo 'repo build failure throws: ';
  var_dump($throws);
  echo 'failed build is not reused: ';
  var_dump($rebuilt);
  echo 'late repo build failure throws: ';
  var_dump($late['throws']);
  echo 'late failure ran the compiler: ';
  var_dump($late['compiled']);
  echo 'late failure removes repo output: ';
  var_dump($late['cleaned']);
  echo 'empty repo directories are removed: ';
  var_dump($cleanup['empty_removed']);
  echo 'unknown diagnostics are retained: ';
  var_dump($cleanup['diagnostic_kept']);
  if ($throws && $rebuilt && $late['throws'] && $late['compiled'] &&
      $late['cleaned'] && $cleanup['empty_removed'] && $cleanup['diagnostic_kept']) {
    // Keep the log, which holds the compiler output, only when this fails.
    try_unlink(ServerUtilServerTests::$LOG_ROOT.'_test'.
               ServerUtilServerTests::test_run_id().'.log');
  }
}
