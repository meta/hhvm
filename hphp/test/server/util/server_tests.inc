<?hh

function try_unlink(string $s): void {
  if (file_exists($s)) {
    unlink($s);
  }
}

function try_readfile(string $s): void {
  if (is_readable($s)) {
    readfile($s);
  }
}

abstract final class ServerUtilServerTests {
  public static $LOG_ROOT;
  public static $DOC_ROOT;
  public static $next_instance_id = 0;
  public static $repo_builds = dict[];
  public static $request = null;

  <<__Memoize>>
  public static function test_run_id() {
    return posix_getpid();
  }

  <<__Memoize>>
  public static function working_dir() {
    return sys_get_temp_dir();
  }

  <<__Memoize>>
  public static function socket_dir() {
    return getenv('HPHP_TEST_SOCKETDIR') ?? sys_get_temp_dir();
  }

  <<__Memoize>>
  public static function error_log_file() {
    return fopen(ServerUtilServerTests::$LOG_ROOT.'_test'.ServerUtilServerTests::test_run_id().".log", 'w');
  }

  public static function getRequest() {
    if (self::$request !== null) {
      return self::$request;
    }
    return http_request<>;
  }
}

function tlog($str) {


  fwrite(ServerUtilServerTests::error_log_file(), $str ?? '');
  fwrite(ServerUtilServerTests::error_log_file(), "\n");
  fflush(ServerUtilServerTests::error_log_file());
  // error_log($str);
}

function dumpLogFilesToStdoutAndDie() {
  $LOG_ROOT = ServerUtilServerTests::$LOG_ROOT;
  $test_run_id = ServerUtilServerTests::test_run_id();

  sleep(1);
  error_log('-------------------------------------------');
  error_log("Contents of '{$LOG_ROOT}_test$test_run_id.log'");
  try_readfile("{$LOG_ROOT}_test$test_run_id.log");
  echo "\n";
  error_log('-------------------------------------------');
  error_log("Contents of '{$LOG_ROOT}_test_server$test_run_id.log'");
  try_readfile("{$LOG_ROOT}_test_server$test_run_id.log");
  echo "\n";
  error_log('-------------------------------------------');
  error_log("Contents of '{$LOG_ROOT}_test_server_stdout$test_run_id.log'");
  try_readfile("{$LOG_ROOT}_test_server_stdout$test_run_id.log");
  echo "\n";
  error_log('-------------------------------------------');
  error_log("Contents of '{$LOG_ROOT}_test_server_stderr$test_run_id.log'");
  try_readfile("{$LOG_ROOT}_test_server_stderr$test_run_id.log");
  echo "\n";
  error_log('-------------------------------------------');
  error_log("Contents of '{$LOG_ROOT}_test_client$test_run_id.log'");
  try_readfile("{$LOG_ROOT}_test_client$test_run_id.log");
  echo "\n";
  error_log('-------------------------------------------');
  error_log("Contents of '{$LOG_ROOT}_test_sandbox_access.log'");
  try_readfile("{$LOG_ROOT}_test_sandbox_access.log");
  echo "\n";
  error_log('-------------------------------------------');
  error_log("Contents of '{$LOG_ROOT}_curl$test_run_id.log'");
  try_readfile("{$LOG_ROOT}_curl$test_run_id.log");
  echo "\n";
  error_log('-------------------------------------------');

  $log_data = file_get_contents("{$LOG_ROOT}_test_server$test_run_id.log");
  $hits = null;
  if (
    $log_data !== false &&
    preg_match_with_matches(
      '#\nStack trace in ([a-zA-Z0-9._/-]+)\n#',
      $log_data,
      inout $hits,
    ) !== 0
  ) {
    for ($i = 1; $i < count($hits); $i++) {
      if (is_readable($hits[$i])) {
        echo "\n";
        error_log('-------------------------------------------');
        error_log("Contents of '{$hits[$i]}'");
        readfile($hits[$i]);
        echo "\n";
        error_log('-------------------------------------------');
      } else {
        error_log("Unable to read {$hits[$i]}");
      }
    }
  }
  throw new Exception("test failed");
}

function hphp_home() {
  // __DIR__ == result.'hphp/test/server/util'
  return realpath(__DIR__.'/../../../..');
}

function get_random_port($exclude1, $exclude2) {
  $BasePort = 20000;
  $PortRange = 3000;
  do {
    $t = rand($BasePort, $BasePort+$PortRange);
  } while ($t == $exclude1 || $t == $exclude2);
  return $t;
}

// Return the command line to start the server with the specified options
function getServerCmd($serverPort, $adminPort, $debugPort, $home, $root,
                      $customArgs = '', $serverId = null, $repoArgs = '') {
  $LOG_ROOT = ServerUtilServerTests::$LOG_ROOT;
  $test_run_id = ServerUtilServerTests::test_run_id();

  $instance_id = ServerUtilServerTests::$next_instance_id;
  ++ServerUtilServerTests::$next_instance_id;

  $portConfig = ' -vServer.Port='.$serverPort;
  $serverConfig = ' --config='.$home.'/config/server.ini';
  $logFileConfig = ' -vLog.File='."{$LOG_ROOT}_test_server$test_run_id.log";
  $logFileConfig.= ' -vLog.Access.Default.File='.
    "{$LOG_ROOT}_access_log$test_run_id.log";
  $srcRootConfig = ' -vServer.SourceRoot='.$root;
  $includePathConfig = ' -vServer.IncludeSearchPaths.0='.$root;
  $adminPortConfig = $adminPort ? ' -vAdminServer.Port='.$adminPort : '';
  $debugPortConfig = $debugPort ? ' -vEval.Debugger.Port='.$debugPort : '';
  $useJit = array_key_exists('HHVM_JIT', \HH\global_get('_ENV')) && \HH\global_get('_ENV')['HHVM_JIT'] == 1;
  $jitConfig = ' -vEval.Jit='.($useJit ? "true" : "false");
  // To emulate sandbox setup, let Sandbox.Home be '$home'
  // and user name be 'debugger', so that the server can find the
  // sandbox_conf.hdf in '$root'.
  $sandboxHomeConfig = ' -vSandbox.Home='.$home;
  $serverId ??= $test_run_id;

  $hhvm = HH\__internal\hhvm_binary();

  if (ini_get('hhvm.repo.authoritative')) {
    if (count(ServerUtilServerTests::$repo_builds) === 0) {
      register_shutdown_function(
         function() {
           foreach (ServerUtilServerTests::$repo_builds as $dir) {
             try_unlink($dir . "/hhvm.hhbc");
             $contents = scandir($dir);
             if (HH\Lib\C\is_empty($contents)) {
               rmdir($dir);
             }
           }
         }
       );
    }
    $repoKey = 'x'.$root.':'.$repoArgs;
    if (!isset(ServerUtilServerTests::$repo_builds[$repoKey])) {
      ServerUtilServerTests::$repo_builds[$repoKey] =
        "{$LOG_ROOT}_bytecode{$test_run_id}_{$instance_id}";
      $cmd = "$hhvm --hphp -l1 -o " . ServerUtilServerTests::$repo_builds[$repoKey] .
        " -vParserThreadCount=1" .
        " -vRuntime.ResourceLimit.CoreFileSize=0" .
        " -vRuntime.Eval.EnableLogBridge=false" .
        " --input-dir $root $repoArgs";

      tlog('Building repo with command: '.$cmd);
      $output = null;
      $return_var = -1;
      tlog(exec($cmd, inout $output, inout $return_var));
    }
    $repoConfig =
      " -vRepo.Path=".ServerUtilServerTests::$repo_builds[$repoKey]."/hhvm.hhbc".
      " -vRepo.Authoritative=true";
  } else {
    $repoConfig = '';
  }

  $cmd = "exec env MALLOC_CONF=junk:true TESTID={$serverId} " .
    "SERVERPORT={$serverPort} $hhvm" .
    ' --mode=server' . $serverConfig . $logFileConfig .
    ' -vServer.ExitOnBindFail=true' .
    ' --instance-id=' . $test_run_id .
    ' -vPageletServer.ThreadCount=5' .
    ' -vPidFile=' . escapeshellarg(sys_get_temp_dir().'/'.'www.pid') .
    $portConfig . $srcRootConfig .
    $includePathConfig . $sandboxHomeConfig . $adminPortConfig .
    $debugPortConfig . $jitConfig . ' ' . $customArgs .
    " > {$LOG_ROOT}_test_server_stdout$test_run_id.log" .
    " 2> {$LOG_ROOT}_test_server_stderr$test_run_id.log";

  return $cmd;
}

function startServer(inout $serverPort, inout $adminPort, inout $debugPort, $home, $root,
                     $customArgs = '', $serverId = null, $repoArgs = '',
                     $expect404 = false) {


  $chooseServer = $serverPort === null;
  $chooseAdmin = $adminPort === null;
  $chooseDebug = $debugPort === null;
  $pid = posix_getpid();
  $output = null;
  $return_var = -1;
  $safe_children = array_flip(array_map(
    $v ==> (int)$v,
    explode(',', exec("pgrep -f -d , -P $pid", inout $output, inout $return_var) ?? ''),
  ));

  for ($i = 0; $i < 5; $i++) {
    if ($chooseServer) $serverPort = get_random_port($adminPort, $debugPort);
    if ($chooseAdmin) $adminPort = get_random_port($serverPort, $debugPort);
    if ($chooseDebug) $debugPort = get_random_port($serverPort, $adminPort);

    $cmd = getServerCmd($serverPort, $adminPort, $debugPort, $home, $root,
                        $customArgs, $serverId, $repoArgs);
    tlog('Starting server with command: '.$cmd);
    $pipes = dict[];
    $serverProc = proc_open($cmd, dict[], inout $pipes);
    if (!is_resource($serverProc)) {
      tlog('Failed to start a shell process for the server');
    } else if (waitForServerToGetGoing($serverPort, $serverProc, $serverId, $expect404)) {
      return $serverProc;
    }
    killChildren($pid, $safe_children);
    if ($serverProc) proc_close($serverProc);
  }
  dumpLogFilesToStdoutAndDie();
}

// Check if the server id is in the expected list of ids.
function checkServerId($serverPort, $expectedIds) {
  $host = 'localhost';
  $r = request($host, $serverPort, "hello.php");
  $matches = null;
  if (preg_match_with_matches('/Hello, World!(.*+)/', $r ?? '', inout $matches)) {
    if (!is_array($expectedIds)) {
      $expectedIds = vec[$expectedIds];
    }
    foreach ($expectedIds as $id) {
      if (HH\Lib\Legacy_FIXME\eq($matches[1], $id)) return true;
    }
    tlog('a server for a different test responded');
    return false;
  }
  tlog('Server replied: '.(string)($r));
  return false;
}

function waitForServerToGetGoing($serverPort, $serverProc, $serverId = null, $expect404 = false) {
  $serverId ??= ServerUtilServerTests::test_run_id();
  for ($i = 1; $i <= 20; $i++) {
    $status = proc_get_status($serverProc);
    if ($status === false || !$status['running']) {
      break;
    }
    sleep(1);
    if ($expect404) {
      $r = request('localhost', $serverPort, "hello.php");
      if ($r === "404 File Not Found") {
        return true;
      }
    } else {
      if (checkServerId($serverPort, $serverId)) {
        return true;
      }
    }
  }

  tlog('Server is not responding.');
  return false;
}

function stopServer($adminPort, $serverProc) {
  $LOG_ROOT = ServerUtilServerTests::$LOG_ROOT;
  $test_run_id = ServerUtilServerTests::test_run_id();


  $r = "";
  for ($i = 1; $i <= 10; $i++) {
    $r = request('localhost', $adminPort, 'stop?instance-id='.$test_run_id);
    if ($r === "OK") break;
    usleep(100000);
  }
  if ($r != "OK") {
    tlog("Server did not stop. Response was $r");
    dumpLogFilesToStdoutAndDie();
  }
  proc_close($serverProc);

  try_unlink("{$LOG_ROOT}_test$test_run_id.log");
  try_unlink("{$LOG_ROOT}_test_server$test_run_id.log");
  try_unlink("{$LOG_ROOT}_test_server_stderr$test_run_id.log");
  try_unlink("{$LOG_ROOT}_test_server_stdout$test_run_id.log");
  try_unlink("{$LOG_ROOT}_test_client$test_run_id.log");
  try_unlink("{$LOG_ROOT}_client$test_run_id.hhbc");
  try_unlink("{$LOG_ROOT}_curl$test_run_id.log");
  try_unlink("{$LOG_ROOT}_access_log$test_run_id.log");

  $next_instance_id = ServerUtilServerTests::$next_instance_id;
  for ($instance_id = 0; $instance_id < $next_instance_id; $instance_id++) {
    try_unlink("{$LOG_ROOT}_server{$test_run_id}_{$instance_id}.hhbc");
  }
}

<<__DynamicallyCallable>>
function http_request($host, $port, $path, $timeout = 1200, $curl_opts = '',
                      $host_prefix = 'hphpd.debugger') {

  if (is_array($path)) {
    $headers = $path[2] ?? null;
    $post = $path[1] ?? null;
    $path = $path[0];
  } else {
    $headers = null;
    $post = null;
  }
  if ($post) {
    $post = http_build_query($post);
    $post = '--data-urlencode='.urlencode($post);;
  }
  if ($headers) {
    $s = vec[];
    foreach ($headers as $h => $v) {
      $s[] = "-H '$h: $v'";
    }
    $headers = implode(" ", $s);
  }
  $url = "http://$host:$port/$path";
  $host_name = "$host_prefix.".\php_uname('n');

  $LOG_ROOT = ServerUtilServerTests::$LOG_ROOT;
  $test_run_id = ServerUtilServerTests::test_run_id();
  $post__str = (string)($post);
  $LOG_ROOT__str = (string)($LOG_ROOT);
  $test_run_id__str = (string)($test_run_id);
  $headers__str = (string)($headers);
  $cmd = "curl $post__str --trace-ascii {$LOG_ROOT__str}_curl$test_run_id__str.log ".
    "--silent $curl_opts --connect-timeout $timeout ".
    "-H 'Host: $host_name' $headers__str --url \"$url\"";
  tlog("Requesting page with command: $cmd");
  $result = null;
  $return_var = -1;
  if (exec($cmd, inout $result, inout $return_var) === null) return null;
  return implode("\n", $result);
}

function requestAll(varray $requests, $customArgs = '', $repoArgs = '') {
  runTest(
    function($serverPort) use ($requests) {
      foreach ($requests as $request) {
        $r = is_array($request) ? $request[0] : $request;
        echo "Requesting '$r'\n";
        var_dump(request('localhost', $serverPort, $request));
      }
    },
    $customArgs,
    $repoArgs,
  );
}

function request($host, $port, $path) {
  $request = ServerUtilServerTests::getRequest();
  return $request($host, $port, $path);
}

function killChildren($pid, $safe_children = dict[]) {
  $output = null;
  $return_var = -1;
  $childIds = exec("pgrep -f -d , -P $pid", inout $output, inout $return_var);
  foreach (array_map($v ==> (int)$v, explode(",", $childIds ?? '')) as $cid) {
    if (!$cid) continue;
    if (isset($safe_children[$cid])) continue;
    tlog("killing ".exec("ps -f -p ".$cid, inout $output, inout $return_var));
    killChildren($cid, $safe_children);
    posix_kill($cid, SIGKILL);
  }
}
