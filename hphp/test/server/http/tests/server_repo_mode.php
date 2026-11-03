<?hh

// A repo-mode run (`-r`) must also run its server repo-authoritatively.
<<__EntryPoint>>
function main(): void {
  require_once('test_base.inc');
  init();
  runTest(
    function($port) {
      $expected = ini_get('hhvm.repo.authoritative') ? 'true' : 'false';
      $actual = request('localhost', $port, 'repo_mode.php');
      tlog("controller repo mode: $expected, server repo mode: $actual");
      echo 'server repo mode matches controller: ';
      var_dump($actual === $expected);
    },
  );
}
