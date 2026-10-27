<?hh
/**
* Run the test suites in various configurations.
*/

use namespace HH\Lib\C;
use namespace HH\Lib\Str;

const int TIMEOUT_SECONDS = 300;

const string MULTI_REQUEST_SEP = '//<>!<>&&|MULTI_REQUEST_SEP|&&<>!<>\\';

function get_argv(): vec<string> {
  return \HH\FIXME\UNSAFE_CAST<vec<mixed>,vec<string>>(
    \HH\global_get('argv') as vec<_>
  );
}

function mtime(): float {
  return microtime(true) as float;
}

// The "HPHP_HOME" environment variable can be set (to ".../fbcode"), to
// define "hphp_home()" and (indirectly) "test_dir()".  Otherwise, we will use
// "__DIR__" as "test_dir()", and its grandparent directory for "hphp_home()"
// (unless we are testing a dso extensions).

<<__Memoize>>
function is_testing_dso_extension(): bool {
  $home = getenv("HPHP_HOME");
  if ($home is string) {
    return false;
  }
  // detecting if we're running outside of the hhvm codebase.
  return !is_file(__DIR__."/../../hphp/test/run.php");
}

<<__Memoize>>
function hphp_home(): string {
  $home = getenv("HPHP_HOME");
  if ($home is string) {
    return realpath($home);
  }
  if (is_testing_dso_extension()) {
    return realpath(__DIR__);
  }
  return realpath(__DIR__."/../..");
}

<<__Memoize>>
function test_dir(): string {
  $home = getenv("HPHP_HOME");
  if ($home is string) {
    return realpath($home)."/hphp/test";
  }
  return __DIR__;
}

function get_expect_file_and_type(
  string $test,
  Options $options,
): vec<?string> {
  $types = vec[
    'expect',
    'expectf',
  ];
  if ($options->repo) {
    if (file_exists($test . '.hphpc_assert')) {
      return vec[$test . '.hphpc_assert', 'expectf'];
    }
    if (file_exists($test . '.hhbbc_assert')) {
      return vec[$test . '.hhbbc_assert', 'expectf'];
    }
    foreach ($types as $type) {
      $fname = "$test.$type-repo";
      if (file_exists($fname)) {
        return vec[$fname, $type];
      }
    }
  }

  foreach ($types as $type) {
    $fname = "$test.$type";
    if (file_exists($fname)) {
      return vec[$fname, $type];
    }
  }
  return vec[null, null];
}

function multi_request_modes(Options $options): vec<string> {
  $r = vec[];
  if ($options->retranslate_all is nonnull) $r[] = 'retranslate-all';
  if ($options->recycle_tc is nonnull) $r[] = 'recycle-tc';
  if ($options->jit_serialize is nonnull) $r[] = 'jit-serialize';
  if ($options->free_random_units is nonnull) $r[] = 'free-random-units';
  if ($options->repeat is nonnull) $r[] = 'repeat';
  if ($options->cli_server) $r[] = 'cli-server';
  return $r;
}

function has_multi_request_mode(Options $options): bool {
  return count(multi_request_modes($options)) != 0;
}

function jit_serialize_option(
  string $cmd, string $test, Options $options, bool $serialize,
): string {
  $serialized = Status::getTestWorkingDir($test) . "/jit.dump";
  $cmds = explode(' -- ', $cmd, 2);
  $jit_serialize = (int)($options->jit_serialize ?? 0);
  $cmds[0] .=
    ' --count=' . ($serialize ? $jit_serialize + 1 : 1) .
    " -vEval.JitSerdesFile=\"" . $serialized . "\"" .
    " -vEval.JitSerdesMode=" . ($serialize ? 'Serialize' : 'DeserializeOrFail') .
    ($serialize ? " -vEval.JitSerializeOptProfRequests=" . $jit_serialize : '');
  if ($options->jitsample is nonnull && $serialize) {
    $cmds[0] .= ' -vDeploymentId="' . $options->jitsample . '-serialize"';
  }
  return implode(' -- ', $cmds);
}

function usage(): string {
  $argv = get_argv();
  return "usage: {$argv[0]} [-m jit|interp] [-r] <test/directories>";
}

function error(string $message): noreturn {
  print "$message\n";
  exit(1);
}

// If a user-supplied path is provided, let's make sure we have a valid
// executable. Returns canonicanalized path or exits.
function check_executable(string $path): string {
  $rpath = realpath($path);
  if ($rpath === false || !is_executable($rpath)) {
    error("Provided HHVM executable ($path) is not an executable file.\n" .
          "If using HHVM_BIN, make sure that is set correctly.");
  }

  $output = vec[];
  $return_var = -1;
  exec($rpath . " --version 2> /dev/null", inout $output, inout $return_var);
  if (strpos(implode("", $output), "HipHop ") !== 0) {
    error("Provided file ($rpath) is not an HHVM executable.\n" .
          "If using HHVM_BIN, make sure that is set correctly.");
  }

  return $rpath;
}

function find_executable(string $name): ?string {
  $output = vec[];
  $return_var = -1;
  exec("which ".escapeshellarg($name)." 2> /dev/null", inout $output, inout $return_var);
  return $return_var === 0 && isset($output[0]) ? $output[0] : null;
}

function hhvm_binary_routes(): dict<string, string> {
  return dict[
    "buck"    => "/buck-out/gen/hphp/hhvm/hhvm",
    "buck2"   => "/../buck-out/v2/gen/fbcode/hphp/hhvm/out",
    "cmake"   => "/hphp/hhvm"
  ];
}

// Return build-route directories in priority order, encoding the selection
// policy once.  If FBCODE_BUILD_TOOL is set, only that tool's route is
// returned; otherwise all routes are returned in definition order.
function build_route_candidates(dict<string, string> $routes): vec<string> {
  $home = hphp_home();
  $env_tool = getenv("FBCODE_BUILD_TOOL");
  if ($env_tool !== false) {
    return vec[$home . $routes[$env_tool]];
  }
  $out = vec[];
  foreach ($routes as $_ => $path) {
    $out[] = $home . $path;
  }
  return $out;
}

// Extract the build-tree prefix from a build output directory — everything
// before the /hphp/ package root.  Works for gen/, art/, and cmake layouts.
// Returns null when the path doesn't look like an hphp build tree.
function build_tree_prefix(string $build_dir): ?string {
  $pos = strpos($build_dir, '/hphp/');
  return $pos !== false ? substr($build_dir, 0, $pos) : null;
}

// Find a binary by probing build-output routes (same mechanism as bin_root).
// When gen/ routes are empty (deferred materialisation), derives the art/
// path from hhvm's build tree using the buck2 route's target subpath.
function find_build_binary(
  string $name,
  dict<string, string> $routes,
): ?string {
  foreach (build_route_candidates($routes) as $dir) {
    $candidate = $dir . '/' . $name;
    if (is_file($candidate)) return $candidate;
  }
  $prefix = build_tree_prefix(bin_root());
  $buck2 = $routes["buck2"] ?? null;
  if ($prefix is nonnull && $buck2 is nonnull) {
    $gen_stem = '/../buck-out/v2/gen/fbcode/';
    if (strpos($buck2, $gen_stem) === 0) {
      $candidate = $prefix . '/' . substr($buck2, strlen($gen_stem))
        . '/' . $name;
      if (is_file($candidate)) return $candidate;
    }
  }
  return null;
}

// For Facebook: We have several build systems, and we can use any of them in
// the same code repo.  If multiple binaries exist, we want the onus to be on
// the user to specify a particular one because before we chose the buck one
// by default and that could cause unexpected results.
function check_for_multiple_default_binaries(): void {
  // Env var we use in testing that'll pick which build system to use.
  if (getenv("FBCODE_BUILD_TOOL") !== false) {
    return;
  }

  $home = hphp_home();
  $found = vec[];
  foreach (hhvm_binary_routes() as $path) {
    $abs_path = $home . $path . "/hhvm";
    if (file_exists($abs_path)) {
      $found[] = $abs_path;
    }
  }

  // emacs hack mode thinks <= is some kind of anchor and messes up
  // parsing after it...
  if (!(count($found) > 1)) return;

  $msg = "Multiple binaries exist in this repo. \n";
  foreach ($found as $bin) {
    $msg .= " - " . $bin . "\n";
  }
  $msg .= "Are you in fbcode?  If so, remove a binary \n"
    . "or use the --hhvm-binary-path option to the test runner. \n"
    . "e.g. test/run --hhvm-binary-path /path/to/binary slow\n";
  error($msg);
}

function hhvm_path(): string {
  $file = "";
  $hhvm_bin = getenv("HHVM_BIN");
  if ($hhvm_bin is string) {
    $file = realpath($hhvm_bin);
  } else {
    $file = bin_root().'/hhvm';
  }

  if (!is_file($file)) {
    if (is_testing_dso_extension()) {
      $candidate = find_executable('hhvm');
      if ($candidate is nonnull) return $candidate;
      error("You need to specify hhvm bin with env HHVM_BIN");
    }

    error("$file doesn't exist. Did you forget to build first?");
  }
  return rel_path($file);
}

function bin_root(): string {
  $hhvm_bin = getenv("HHVM_BIN");
  if ($hhvm_bin is string) {
    return dirname(realpath($hhvm_bin));
  }

  $candidates = build_route_candidates(hhvm_binary_routes());

  // When FBCODE_BUILD_TOOL selects a specific route, return it
  // unconditionally (matching existing behavior).
  if (getenv("FBCODE_BUILD_TOOL") !== false) {
    return $candidates[0];
  }

  foreach ($candidates as $dir) {
    if (is_dir($dir)) {
      return $dir;
    }
  }

  return hphp_home() . hhvm_binary_routes()["cmake"];
}

function process_opts(
  string $opts,
  string $dir,
  ?string $input_dir = null,
): string {
  $contents = "";
  foreach (explode("\n", $opts) as $line) {
    // Compress out white space. The trailing space stands in for the newline
    // that separated this line from the next, keeping the options apart.
    $line = preg_replace('/\s+/', ' ', $line . "\n");

    // Discard simple line oriented ; and # comments to end of line
    // Comments at end of line (after payload) are not allowed.
    $line = preg_replace('/^ *;.*$/', ' ', $line);
    $line = preg_replace('/^ *#.*$/', ' ', $line);

    // Substitute in the directory name
    // If input_dir is set, strip it from the expansion of DIR unless
    // the option is -c, which is not affected by --input-dir
    if ($input_dir != null && strpos($line, '-c ') == false) {
      $line = str_replace('__DIR__', realpath($dir), $line);
      if (strpos($line, $input_dir.'/') != false) {
        $line = str_replace($input_dir.'/', '', $line);
      } else {
        // --foo=__DIR__ must be substituted with --foo=/, not --foo=
        $line = str_replace($input_dir, '/', $line);
      }
    } else {
      $line = str_replace('__DIR__', $dir, $line);
    }
    $contents .= $line;
  }
  return $contents;
}

function read_opts_file(?string $file, ?string $input_dir = null): string {
  if ($file is null || !file_exists($file)) {
    return "";
  }
  return process_opts(file_get_contents($file), dirname($file), $input_dir);
}

// Keys understood in a `.config` file.
const keyset<string> TEST_CONFIG_KEYS = keyset[
  // Blocks of command-line options.
  'opts', 'hphp_opts', 'hhbbc_opts', 'sbcc_opts',
  // Booleans, written as `key = true` or `key = false`.
  'norepo', 'noserver', 'nowebserver', 'nomultireq', 'nodumphhas',
  'nojitserialize', 'nobespoke', 'onlyrepo', 'onlyjumpstart', 'onlyfacebook',
  'skip_persistent', 'no_remote', 'uses_loopback', 'serial', 'verify',
  // Free-form values.
  'env', 'clisrv_env', 'exit_code',
];

// Keys that may also appear in a directory-level `config.config`, supplying a
// default for every test in the suite. Everything else describes a single test
// and is rejected there.
const keyset<string> TEST_CONFIG_SUITE_KEYS = keyset['opts', 'hphp_opts'];

// Keys holding a boolean rather than a value.
const keyset<string> TEST_CONFIG_FLAG_KEYS = keyset[
  'norepo', 'noserver', 'nowebserver', 'nomultireq', 'nodumphhas',
  'nojitserialize', 'nobespoke', 'onlyrepo', 'onlyjumpstart', 'onlyfacebook',
  'skip_persistent', 'no_remote', 'uses_loopback', 'serial', 'verify',
];

// Parse a `.config` file, which replaces the per-key sidecar option files.
// Each entry is `key = value`, where the value is either the remainder of the
// line or a block of following indented lines:
//
//   opts = -vEval.Foo=1
//   hphp_opts =
//     --inputs=__DIR__/a.inc
//     --inputs=__DIR__/b.inc
//   norepo = true
//
// Lines whose first non-space character is `;` or `#` are comments.
<<__Memoize>>
function parse_config_file(string $file): dict<string, string> {
  // A directory-level `config.config` supersedes the `config.<key>` sidecar
  // files that still exist alongside it.
  $prefix = substr($file, 0, strlen($file) - strlen('.config'));
  $is_dir_config = basename($file) === 'config.config';

  $config = dict[];
  $key = null;
  $lineno = 0;
  foreach (explode("\n", file_get_contents($file)) as $line) {
    $lineno++;
    $trimmed = trim($line);
    if ($trimmed === '' || $trimmed[0] === ';' || $trimmed[0] === '#') continue;

    if ($line[0] === ' ' || $line[0] === "\t") {
      if ($key is null) {
        error("$file:$lineno: indented line before any 'key = value' line");
      }
      $config[$key] =
        $config[$key] === '' ? $trimmed : $config[$key]."\n".$trimmed;
      continue;
    }

    $matches = vec[];
    if (preg_match_with_matches(
          '/^([A-Za-z_][A-Za-z0-9_]*) *= *(.*)$/',
          $trimmed,
          inout $matches,
        ) !== 1) {
      error("$file:$lineno: expected 'key = value', got '$trimmed'");
    }
    $key = $matches[1];
    if (!C\contains_key(TEST_CONFIG_KEYS, $key)) {
      error(
        "$file:$lineno: unknown key '$key', expected one of ".
        implode(', ', vec(TEST_CONFIG_KEYS))
      );
    }
    if (C\contains_key($config, $key)) {
      error("$file:$lineno: duplicate key '$key'");
    }
    if (is_file("$prefix.$key")) {
      error(
        "$file:$lineno: '$key' is set here and in $prefix.$key; ".
        "remove one of them"
      );
    }
    $value = rtrim($matches[2]);
    if ($is_dir_config && !C\contains_key(TEST_CONFIG_SUITE_KEYS, $key)) {
      error("$file:$lineno: '$key' applies to a single test, not a suite");
    }
    if (C\contains_key(TEST_CONFIG_FLAG_KEYS, $key) &&
        $value !== 'true' && $value !== 'false') {
      error("$file:$lineno: '$key' must be 'true' or 'false', got '$value'");
    }
    $config[$key] = $value;
  }
  return $config;
}

<<__Memoize>>
function read_test_config(string $test): dict<string, string> {
  // The per-test sidecar files are gone; refuse to silently ignore a stale one.
  foreach (TEST_CONFIG_KEYS as $key) {
    if (is_file("$test.$key")) {
      error(
        "$test.$key is no longer read; move its contents into a '$key' ".
        "entry in $test.config"
      );
    }
  }
  $file = "$test.config";
  return is_file($file) ? parse_config_file($file) : dict[];
}

// Read a boolean entry (see TEST_CONFIG_FLAG_KEYS) from `<test>.config`.
function read_test_flag(string $test, string $key): bool {
  return idx(read_test_config($test), $key) === 'true';
}

// Read a free-form entry from `<test>.config`, null when the test omits it.
function read_test_value(string $test, string $key): ?string {
  return idx(read_test_config($test), $key);
}

// Suite-wide default for $key: the nearest enclosing `config.config` that
// defines it, else the legacy `config.<key>` file. The search is per-key so a
// nested `config.config` setting only `opts` does not hide an `hphp_opts`
// inherited from further up, matching how the sidecar files were looked up.
function read_dir_opts(
  string $dir,
  string $key,
  ?string $input_dir = null,
): string {
  $file = find_file_for_dir(
    $dir,
    'config.config',
    $f ==> C\contains_key(parse_config_file($f), $key),
  );
  if ($file is nonnull) {
    return process_opts(
      parse_config_file($file)[$key],
      dirname($file),
      $input_dir,
    );
  }
  return read_opts_file(find_file_for_dir($dir, "config.$key"), $input_dir);
}

// Force the `config.config` lookups a worker will do later, so a malformed
// suite-wide file is reported during test collection instead. Memoized per
// directory because thousands of tests share a handful of config files.
<<__Memoize>>
function validate_dir_config(string $dir): void {
  foreach (TEST_CONFIG_SUITE_KEYS as $key) {
    read_dir_opts($dir, $key);
  }
}

// Read an option block for $test from `<test>.config`. When $dir_fallback is
// set, a test that doesn't set $key falls back to the suite-wide default from
// the enclosing directories.
function read_test_opts(
  string $test,
  string $key,
  ?string $input_dir = null,
  bool $dir_fallback = true,
): string {
  $config = read_test_config($test);
  if (C\contains_key($config, $key)) {
    return process_opts($config[$key], dirname($test), $input_dir);
  }
  return $dir_fallback ? read_dir_opts(dirname($test), $key, $input_dir) : "";
}

function get_test_env(
  Options $options,
  string $test,
  dict<string, mixed> $env,
): dict<string, mixed> {
  $extra_env = read_test_value($test, 'env');
  if ($options->cli_server) {
    $extra_env = read_test_value($test, 'clisrv_env') ?? $extra_env;
  }

  if ($extra_env is nonnull) {
    foreach (explode("\n", $extra_env) as $line) {
      if (trim($line) === '') continue;
      $line = str_replace('__DIR__', realpath(dirname($test)), $line);
      $i = strpos($line, '=');
      if ($i) {
        $key = substr($line, 0, $i);
        $val = substr($line, $i + 1);
        $env[$key] = $val;
      } else {
        unset($env[$line]);
      }
    }
  }

  return $env;
}

// http://stackoverflow.com/questions/2637945/
function rel_path(string $to): string {
  $from     = explode('/', getcwd().'/');
  $to       = explode('/', $to);
  $from_len = count($from);
  $to_len   = count($to);

  // find first non-matching dir.
  for ($d = 0; $d < $from_len; ++$d) {
    if ($d >= $to_len || $from[$d] !== $to[$d])
      break;
  }

  $relPath = vec[];

  // get number of remaining dirs in $from.
  $remaining = $from_len - $d - 1;
  if ($remaining > 0) {
    // add traversals up to first matching dir.
    do {
      $relPath[] = '..';
      $remaining--;
    } while ($remaining > 0);
  } else {
    $relPath[] = '.';
  }
  while ($d < $to_len) {
    $relPath[] = $to[$d];
    $d++;
  }
  return implode('/', $relPath);
}

// Keep this in sync with the dict in get_options() below.
// Options taking a value (with a trailing `:` in the dict key)
// should be ?string. Otherwise they should be bool.
final class Options {
    public ?string $env;
    public ?string $exclude;
    public ?string $exclude_pattern;
    public ?string $exclude_recorded_failures;
    public ?string $include;
    public ?string $include_pattern;
    public bool $repo = false;
    public bool $split_hphpc = false;
    public bool $repo_single = false;
    public bool $repo_separate = false;
    public bool $only_remote_executable = false;
    public bool $only_remote_executable_with_loopback = false;
    public bool $only_non_remote_executable = false;
    public ?string $repo_threads;
    public bool $hhbbc2 = false;
    public ?string $mode;
    public bool $server = false;
    public bool $cli_server = false;
    public bool $shuffle = false;
    public bool $help = false;
    public bool $verbose = false;
    public bool $testpilot = false;
    public ?string $threads;
    public ?string $args;
    public ?string $compiler_args;
    public bool $log = false;
    public ?string $failure_file;
    public bool $wholecfg = false;
    public bool $hhas_round_trip = false;
    public bool $color = false;
    public bool $no_fun = false;
    public bool $no_skipif = false;
    public bool $cores = false;
    public bool $dump_tc = false;
    public bool $no_clean = false;
    public bool $list_tests = false;
    public ?string $recycle_tc;
    public ?string $retranslate_all;
    public bool $async_jit_profile = false;
    public ?string $jit_serialize;
    public ?string $free_random_units;
    public ?string $repeat;
    public ?string $hhvm_binary_path;
    public ?string $working_dir;
    public ?string $vendor;
    public ?string $record_failures;
    public ?string $ignore_oids;
    public ?string $jitsample;
    public ?string $hh_single_type_check;
    public bool $bespoke = false;
    public bool $factsdir = false;
    public bool $persistent = false;

    // Additional state added for convenience since Options is plumbed
    // around almost everywhere.
    public bool $facebook_build = false;
    public ?Servers $servers = null;
}

function get_options(
  vec<string> $argv,
): (Options, vec<string>) {
  // Options marked * affect test behavior, and need to be reported by list_tests.
  // Options with a trailing : take a value.
  $parameters = dict[
    '*env:' => '',
    'exclude:' => 'e:',
    'exclude-pattern:' => 'E:',
    'exclude-recorded-failures:' => 'x:',
    'include:' => 'i:',
    'include-pattern:' => 'I:',
    '*repo' => 'r',
    '*repo-single' => '',
    '*repo-separate' => '',
    '*repo-threads:' => '',
    '*repo-out:' => '',
    '*hhbbc2' => '',
    '*mode:' => 'm:',
    '*server' => 's',
    '*cli-server' => 'S',
    'shuffle' => '',
    'help' => 'h',
    'verbose' => 'v',
    'testpilot' => '',
    'only-remote-executable' => '',
    'only-remote-executable-with-loopback' => '',
    'only-non-remote-executable' => '',
    'threads:' => '',
    '*args:' => 'a:',
    '*compiler-args:' => '',
    'log' => 'l',
    'failure-file:' => '',
    '*wholecfg' => '',
    '*hhas-round-trip' => '',
    'color' => 'c',
    'no-fun' => '',
    'no-skipif' => '',
    'cores' => '',
    'dump-tc' => '',
    'no-clean' => '',
    'list-tests' => '',
    '*recycle-tc:' => '',
    '*retranslate-all:' => '',
    '*async-jit-profile' => '',
    '*free-random-units:' => '',
    '*repeat:' => '',
    '*jit-serialize:' => '',
    '*hhvm-binary-path:' => 'b:',
    '*working-dir:' => 'w:',
    'record-failures:' => '',
    '*ignore-oids' => '',
    'jitsample:' => '',
    '*hh_single_type_check:' => '',
    'write-to-checkout' => '',
    'bespoke' => '',
    '*factsdir' => '',
    '*persistent' => '',
  ];
  $options = new Options() as dynamic;
  $files = vec[];
  $recorded = vec[];

  /*
   * '-' argument causes all future arguments to be treated as filenames, even
   * if they would otherwise match a valid option. Otherwise, arguments starting
   * with '-' MUST match a valid option.
   */
  $force_file = false;

  for ($i = 1; $i < count($argv); $i++) {
    $arg = $argv[$i];

    if (strlen($arg) === 0) {
      continue;
    } else if ($force_file) {
      $files[] = $arg;
    } else if ($arg === '-') {
      $forcefile = true;
    } else if ($arg[0] === '-') {
      $found = false;

      foreach ($parameters as $long => $short) {
        if ($arg == '-'.str_replace(':', '', $short) ||
            $arg == '--'.str_replace(vec[':', '*'], vec['', ''], $long)) {
          $record = substr($long, 0, 1) === '*';
          if ($record) $recorded[] = $arg;
          if (substr($long, -1, 1) === ':') {
            $i++;
            $value = $argv[$i];
            if ($record) $recorded[] = $value;
          } else {
            $value = true;
          }
          $name = str_replace(vec[':', '*', '-'], vec['', '', '_'], $long);
          $options->{$name} = $value;
          $found = true;
          break;
        }
      }

      if (!$found) {
        $msg = sprintf("Invalid argument: '%s'\nSee %s --help", $arg, $argv[0]);
        error($msg);
      }
    } else {
      $files[] = $arg;
    }
  }
  $options = $options as Options;

  \HH\global_set('recorded_options', $recorded);

  if ($options->hhbbc2) {
    $options->repo_separate = true;
    if ($options->repo || $options->repo_single) {
      error("repo-single/repo and hhbbc2 are mutually exclusive options");
    }
    if (isset(HH\FIXME\UNSAFE_CAST<mixed,dynamic>($options)['mode'])) {
      error("hhbbc2 doesn't support modes; it compares hhas, doesn't run code");
    }
  }

  if ($options->repo_single || $options->repo_separate) {
    $options->repo = true;
  } else if ($options->repo) {
    // if only repo was set, then it means repo single
    $options->repo_single = true;
  }

  if ($options->jit_serialize is nonnull) {
    if (!$options->repo) {
      error("jit-serialize only works in repo mode");
    }
    if ($options->mode is nonnull && $options->mode !== 'jit') {
      error("jit-serialize only works in jit mode");
    }
  }

  if ($options->repo && $options->hhas_round_trip) {
    error("repo and hhas-round-trip are mutually exclusive options");
  }

  $multi_request_modes = multi_request_modes($options);
  if (count($multi_request_modes) > 1) {
    error("The options\n -" . implode("\n -", $multi_request_modes) .
          "\nare mutually exclusive options");
  }

  return tuple($options, $files);
}

/*
 * Return the path to $test relative to $base, or false if $base does not
 * contain test.
 */
function canonical_path_from_base(string $test, string $base): mixed {
  $full = realpath($test);
  if (substr($full, 0, strlen($base)) === $base) {
    return substr($full, strlen($base) + 1);
  }
  $dirstat = stat($base);
  if (!is_dict($dirstat)) return false;
  for ($p = dirname($full); $p !== "" && $p !== "0" && $p !== "/"; $p = dirname($p)) {
    $s = stat($p);
    if (!is_dict($s)) continue;
    if ($s['ino'] === $dirstat['ino'] && $s['dev'] === $dirstat['dev']) {
      return substr($full, strlen($p) + 1);
    }
  }
  return false;
}

function canonical_path(string $test): mixed {
  $attempt = canonical_path_from_base($test, test_dir());
  if ($attempt === false) {
    return canonical_path_from_base($test, hphp_home());
  } else {
    return $attempt;
  }
}

/*
 * We support some 'special' file names, that just know where the test
 * suites are, to avoid typing 'hphp/test/foo'.
 */
function find_test_files(string $file): vec<string>{
  $mappage = dict[
    'quick'    => 'hphp/test/quick',
    'slow'     => 'hphp/test/slow',
    'debugger' => 'hphp/test/server/debugger/tests',
    'http'     => 'hphp/test/server/http/tests',
    'fastcgi'  => 'hphp/test/server/fastcgi/tests',
    'zend'     => 'hphp/test/zend/good',
    'facebook' => 'hphp/facebook/test',

    // subset of slow we run with CLI server too
    'slow_ext_hsl' => 'hphp/test/slow/ext_hsl',

    // Subsets of zend tests.
    'zend_ext'    => 'hphp/test/zend/good/ext',
    'zend_Zend'   => 'hphp/test/zend/good/Zend',
    'zend_tests'  => 'hphp/test/zend/good/tests',
    // Extension tests
    'crypto_keyset' => 'hphp/facebook/extensions/crypto_keyset/test',
    'cryptonative' => 'hphp/facebook/extensions/cryptonative/test',
    'thrift' => 'hphp/facebook/extensions/thrift/test',
    'flighttracker' => 'hphp/facebook/extensions/flighttracker/test',
    'fci' => 'hphp/facebook/extensions/fci/test',
    'scribe' => 'hphp/facebook/extensions/scribe/test',
    'dataflowmonitor' => 'hphp/facebook/extensions/dataflowmonitor/test',
  ];

  $pattern = $mappage[$file] ?? null;
  if ($pattern is nonnull) {
    $pattern = hphp_home().'/'.$pattern;
    $matches = glob($pattern);
    if (count($matches) === 0) {
      error(
        "Convenience test name '$file' is recognized but does not match ".
        "any test files (pattern = '$pattern')",
      );
    }
    return $matches;
  }

  return vec[$file];
}

// Some tests have to be run together in the same test bucket, serially, one
// after other in order to avoid races and other collisions.
function serial_only_tests(vec<string> $tests): vec<string> {
  if (is_testing_dso_extension()) {
    return vec[];
  }
  // Add a <testname>.php.serial file to make your test run in the serial
  // bucket.
  $serial_tests = vec(array_filter(
    $tests,
    function($test) {
      return read_test_flag($test, 'serial');
    }
  ));
  return $serial_tests;
}

// If "files" is very long, then the shell may reject the desired
// "find" command (especially because "escapeshellarg()" adds two single
// quote characters to each file), so we split "files" into chunks below.
function exec_find(vec<string> $files, string $extra): vec<string> {
  $results = vec[];
  foreach (array_chunk($files, 500) as $chunk) {
    $efa = implode(' ', array_map(
      /* HH_IGNORE[12012] $line is an int so the `as` always fails */
      $line ==> escapeshellarg($line as string),
      $chunk as dict<_, _>,
    ));
    $output = shell_exec("find $efa $extra");
    foreach (explode("\n", $output) as $result) {
      // Collect the (non-empty) results, which should all be file paths.
      if ($result !== "") $results[] = $result;
    }
  }
  return $results;
}

function is_facebook_build(Options $options): bool {
  if (!is_dir(hphp_home() . '/hphp/facebook/test')) return false;

  // We want to test for the presence of an extension in the build, so turn off
  // a bunch of features in order to do this as simply and reliably as possible.
  $simplified_options = clone $options;
  $simplified_options->repo = false;
  $simplified_options->server = false;
  $simplified_options->cli_server = false;
  // Use a bogus test name so we don't find any config overrides
  $result = runif_extension_matches(
    $simplified_options,
    'not_a_real_test.php',
    vec['facebook'],
  );
  if (!$result['valid']) {
    invariant(Shapes::keyExists($result, 'error'), 'RunifResult contract');
    invariant_violation(
      "is_facebook_build is calling runif_extension_matches incorrectly: %s",
      $result['error'],
    );
  }
  invariant(Shapes::keyExists($result, 'match'), 'RunifResult contract');
  return $result['match'];
}

function find_tests(
  vec<string> $files,
  Options $options,
): vec<string> {
  if ($files === vec[]) {
    $files = vec['quick'];
  }
  if ($files == vec['all']) {
    $files = vec['quick', 'slow', 'zend', 'fastcgi', 'http', 'debugger'];
    if ($options->facebook_build) {
      $files[] = 'facebook';
    }
  }
  $ft = vec[];
  foreach ($files as $file) {
    $ft = array_merge($ft, find_test_files($file));
  }
  $files = vec[];
  foreach ($ft as $file) {
    if (!file_exists($file)) {
      error("Not valid file or directory: '$file'");
    }
    $file = preg_replace(',//+,', '/', realpath($file));
    $file = preg_replace(',^'.getcwd().'/,', '', $file);
    $files[] = $file;
  }
  $tests = exec_find(
    $files,
    "'(' " .
    "-name '*.php' " .
    "-o -name '*.hack' " .
    "-o -name '*.hackpartial' " .
    "-o -name '*.hhas' " .
    "-o -name '*.php.type-errors' " .
    "-o -name '*.hack.type-errors' " .
    "-o -name '*.hackpartial.type-errors' " .
    "')' " .
    "-not -regex '.*round_trip[.]hhas' " .
    "-not -name '*.inc.php' " .
    "-not -path '*.sbcc_build_root*' " .
    "-not -path '*.sbcc_run_root*'"
  );
  if ($tests === vec[]) {
    error("Could not find any tests associated with your options.\n" .
          "Make sure your test path is correct and that you have " .
          "the right expect files for the tests you are trying to run.\n" .
          usage());
  }
  asort(inout $tests);
  $tests = vec(array_filter($tests));
  if ($options->exclude is nonnull) {
    $exclude = $options->exclude;
    $tests = vec(array_filter($tests, function($test) use ($exclude) {
      return (false === strpos($test, $exclude));
    }));
  }
  if ($options->exclude_pattern is nonnull) {
    $exclude = $options->exclude_pattern;
    $tests = vec(array_filter($tests, function($test) use ($exclude) {
      return preg_match($exclude, $test) !== 1;
    }));
  }
  if ($options->exclude_recorded_failures is nonnull) {
    $exclude_file = $options->exclude_recorded_failures;
    $exclude = file($exclude_file, FILE_IGNORE_NEW_LINES);
    $tests = vec(array_filter($tests, function($test) use ($exclude) {
      return (false === in_array(canonical_path($test), $exclude));
    }));
  }

  if ($options->list_tests) {
    $tests = vec(array_filter($tests, function($test) use ($options) {
      return should_skip_test_simple($options, $test) is null;
    }));
  }

  if ($options->include is nonnull) {
    $include = $options->include;
    $tests = vec(array_filter($tests, function($test) use ($include) {
      return (false !== strpos($test, $include));
    }));
  }
  if ($options->include_pattern is nonnull) {
    $include = $options->include_pattern;
    $tests = vec(array_filter($tests, function($test) use ($include) {
      return preg_match($include, $test) === 1;
    }));
  }
  // Parse up front so a malformed .config file is reported here rather than
  // from inside a worker, where it would look like an ordinary test failure.
  foreach ($tests as $test) {
    read_test_config($test);
    validate_dir_config(dirname($test));
  }
  return $tests;
}

function list_tests(vec<string> $files, Options $options): void {
  $args = implode(' ', \HH\global_get('recorded_options'));

  // Disable escaping of test info when listing. We check if the environment
  // variable is set so we can make the change in a backwards compatible way.
  $escape_info = getenv("LISTING_NO_ESCAPE") === false;

  foreach (find_tests($files, $options) as $test) {
    $test_info = Status::jsonEncode(
        dict['args' => $args, 'name' => $test],
    );
    if ($escape_info) {
        print str_replace('\\', '\\\\', $test_info)."\n";
    } else {
        print $test_info."\n";
    }
  }
}

function find_test_ext(
  string $test,
  string $ext,
  string $configName='config',
): ?string {
  if (is_file("{$test}.{$ext}")) {
    return "{$test}.{$ext}";
  }
  return find_file_for_dir(dirname($test), "{$configName}.{$ext}");
}

// Find $name in $dir or the nearest enclosing directory. $accept lets a caller
// keep searching upwards past a file that exists but isn't the one it wants.
function find_file_for_dir(
  string $dir,
  string $name,
  ?(function(string): bool) $accept = null,
): ?string {
  // Handle the case where the $dir might come in as '.' because you
  // are running the test runner on a file from the same directory as
  // the test e.g., './mytest.php'. dirname() will give you the '.' when
  // you actually have a lot of path to traverse upwards like
  // /home/you/code/tests/mytest.php. Don't use realpath() to get that because
  // it will mess up relative paths.
  $depth = count(explode('/', realpath($dir))) - 1;
  for (; $dir !== '/' && is_dir($dir) && $depth !== 0; $depth--) {
    $file = "$dir/$name";
    if (is_file($file) && ($accept is null || $accept($file))) {
      return $file;
    }
    if ($dir === '.' || substr($dir, -2) === '..') {
      $dir = $dir . '/..';
    } else {
      $dir = dirname($dir);
    }
  }
  $file = test_dir().'/'.$name;
  if (file_exists($file) && ($accept is null || $accept($file))) {
    return $file;
  }
  return null;
}

function find_set_input_dir(string $test): ?string {
  $set_input_dir = find_file_for_dir($test, ".set_input_dir");
  if ($set_input_dir is nonnull) {
    return dirname($set_input_dir);
  }
  return null;
}

function find_debug_config(string $test, string $name): string {
  $debug_config = find_file_for_dir(dirname($test), $name);
  if ($debug_config is nonnull) {
    return "-m debug --debug-config ".$debug_config;
  }
  return "";
}

///////////////////////////////////////////////////////////////////////////////
// SBCC test metadata helpers.
//
// SBCC tests use runner-owned metadata files to declare build or fixture
// inputs. The runner materializes the .sbcc artifact before HHVM starts,
// parallel to how repo-mode tests build .hhbc before runtime.

function sbcc_enabled(string $test): bool {
  return is_file("$test.sbcc_files") || is_file("$test.sbcc_fixture");
}

function sbcc_uses_builder(string $test): bool {
  return is_file("$test.sbcc_files");
}

function sbcc_binary_routes(): dict<string, string> {
  return dict[
    "buck"  => "/buck-out/gen/hphp/runtime/ext/sbcc/sbcc-build",
    "buck2" => "/../buck-out/v2/gen/fbcode/hphp/runtime/ext/sbcc/__sbcc-build__/out",
    "cmake" => "/hphp/runtime/ext/sbcc",
  ];
}

<<__Memoize>>
function sbcc_build_binary(): ?string {
  // SBCC_BUILD is set by the buck2 test infrastructure in DEFS.bzl via
  // $(location //hphp/runtime/ext/sbcc:sbcc-build).
  $env = getenv('SBCC_BUILD');
  if ($env is string && is_file($env)) return $env;
  // Check alongside hhvm, for when both binaries were copied to the same
  // directory (e.g. /tmp/).
  $candidate = dirname(hhvm_path()).'/sbcc-build';
  if (is_file($candidate)) return $candidate;
  // Probe build routes — same infrastructure as bin_root() / hhvm_path().
  $found = find_build_binary('sbcc-build', sbcc_binary_routes());
  if ($found is nonnull) return $found;
  return find_executable('sbcc-build');
}

function sbcc_working_artifact(string $test): string {
  return Status::getTestWorkingDir($test).'/test.sbcc';
}

function sbcc_working_build_root(string $test): string {
  return Status::getTestWorkingDir($test).'/sbcc-build-root';
}

function sbcc_working_run_root(string $test): string {
  return Status::getTestWorkingDir($test).'/sbcc-run-root';
}

function sbcc_copy_tree(string $src, string $dst): void {
  if (!is_dir($src)) {
    throw new Exception("SBCC: source directory does not exist: $src");
  }
  $cmd = 'cp -a '.escapeshellarg($src).' '.escapeshellarg($dst);
  $ret = 0;
  $output = vec[];
  exec($cmd, inout $output, inout $ret);
  if ($ret !== 0) {
    throw new Exception("SBCC: failed to copy $src to $dst");
  }
}

function sbcc_mode_setup(Options $options, string $test): (string, bool) {
  try {
    return sbcc_mode_setup_impl($options, $test);
  } catch (\Exception $e) {
    return tuple("SBCC setup error: " . $e->getMessage(), false);
  }
}

function sbcc_mode_setup_impl(Options $options, string $test): (string, bool) {
  $working_dir = Status::getTestWorkingDir($test);

  if (sbcc_uses_builder($test)) {
    // Build-backed test: build .sbcc from source files.
    $build_root_src = "$test.sbcc_build_root";
    if (!is_dir($build_root_src)) {
      return tuple("SBCC: missing .sbcc_build_root/ for $test", false);
    }

    $build_root = sbcc_working_build_root($test);
    $run_root = sbcc_working_run_root($test);

    // Clean and copy build root.
    $rm_out = vec[];
    $rm_ret = 0;
    exec('rm -rf '.escapeshellarg($build_root).' '.escapeshellarg($run_root),
         inout $rm_out, inout $rm_ret);
    sbcc_copy_tree($build_root_src, $build_root);

    // Copy run root (or reuse build root for runtime).
    // When no separate run root is specified, the build root IS the run root.
    // This ensures RepoOptions::forFile() resolves identically and mangled
    // SHA1s match between builder and runtime.
    $run_root_src = "$test.sbcc_run_root";
    if (is_dir($run_root_src)) {
      sbcc_copy_tree($run_root_src, $run_root);
    } else {
      // No separate run root — use the build root for runtime too.
      $run_root = $build_root;
    }

    // Record the actual run root path for hhvm_cmd to use.
    file_put_contents("$working_dir/sbcc_run_root_path", $run_root);

    // Copy file list.
    $file_list_src = "$test.sbcc_files";
    $file_list = "$working_dir/sbcc_files.txt";
    copy($file_list_src, $file_list);

    // Read optional builder opts.
    $sbcc_opts = trim(read_test_opts($test, 'sbcc_opts', null, false));

    // Build the .sbcc artifact.
    // The builder must use the same config values that affect
    // mangleUnitSha1() (CONFIGS_FOR_UNITCACHEFLAGS). Pass the same -v
    // flags that hhvm_cmd_impl adds to runtime, plus the suite-wide opts
    // from the test directory tree.
    $config_v_flags = vec[
      '-v', 'Eval.EnableIntrinsicsExtension=true',
      '-v', 'Eval.FoldLazyClassKeys=false',
    ];
    // Also pass the suite-wide opts (e.g. -vEval.AllowHhas=true).
    // These are in "-vName=Value" format — convert to "-v Name=Value".
    foreach (preg_split('/\s+/', read_dir_opts(dirname($test), 'opts')) as $opt) {
      $m = vec[];
      if (preg_match_with_matches('/^-v(.+)$/', $opt, inout $m) === 1) {
        $config_v_flags[] = '-v';
        $config_v_flags[] = $m[1];
      }
    }

    $artifact = sbcc_working_artifact($test);
    $builder = sbcc_build_binary();
    invariant($builder is nonnull, 'SBCC builder availability checked earlier');
    $cmd_parts = vec[];
    $cmd_parts[] = escapeshellarg($builder);
    $cmd_parts[] = '--output';
    $cmd_parts[] = escapeshellarg($artifact);
    $cmd_parts[] = '--root';
    $cmd_parts[] = escapeshellarg($build_root.'/');
    $cmd_parts[] = '--file-list';
    $cmd_parts[] = escapeshellarg($file_list);
    $cmd_parts[] = '--skip-missing';
    foreach ($config_v_flags as $flag) {
      $cmd_parts[] = $flag;
    }
    if ($sbcc_opts !== '') {
      $cmd_parts[] = $sbcc_opts;
    }
    $cmd_parts[] = '2>&1';
    $cmd = implode(' ', $cmd_parts);

    $output = vec[];
    $ret = 0;
    exec($cmd, inout $output, inout $ret);
    $output_str = implode("\n", $output);

    if ($ret !== 0 || !is_file($artifact)) {
      return tuple("SBCC build failed (exit=".(int)$ret."):\n$output_str", false);
    }

    return tuple($output_str, true);

  } else {
    // Fixture-backed test: copy committed fixture.
    $fixture_src = "$test.sbcc_fixture";
    if (!is_file($fixture_src)) {
      return tuple("SBCC: missing .sbcc_fixture for $test", false);
    }

    $run_root_src = "$test.sbcc_run_root";
    if (!is_dir($run_root_src)) {
      return tuple("SBCC: missing .sbcc_run_root/ for $test", false);
    }

    $run_root = sbcc_working_run_root($test);
    $rm_out2 = vec[];
    $rm_ret2 = 0;
    exec('rm -rf '.escapeshellarg($run_root), inout $rm_out2, inout $rm_ret2);
    sbcc_copy_tree($run_root_src, $run_root);

    $artifact = sbcc_working_artifact($test);
    copy($fixture_src, $artifact);

    // Record the actual run root path for hhvm_cmd to use.
    file_put_contents("$working_dir/sbcc_run_root_path", $run_root);

    return tuple("", true);
  }
}
///////////////////////////////////////////////////////////////////////////////

function mode_cmd(Options $options): vec<string> {
  $repo_args = '';
  $interp_args = "$repo_args -vEval.Jit=0";
  $jit_args = "$repo_args -vEval.Jit=true";
  $mode = $options->mode ?? '';
  switch ($mode) {
    case '':
    case 'jit':
      return vec[$jit_args];
    case 'interp':
      return vec[$interp_args];
    case 'interp,jit':
      return vec[$interp_args, $jit_args];
    default:
      error("-m must be one of jit | interp | interp,jit. Got: '$mode'");
  }
}

function extra_compiler_args(Options $options): string {
  return $options->compiler_args ?? '';
}

function hhvm_cmd_impl(
  Options $options,
  string $test,
  string $config,
  ?string $autoload_db_prefix,
  string ...$extra_args
): vec<string> {
  $cmds = vec[];
  foreach (mode_cmd($options) as $mode_num => $mode) {
    $args = vec[
      hhvm_path(),
      '-c',
      $config,
      // EnableArgsInBacktraces disables most of HHBBC's DCE optimizations.
      // In order to test those optimizations (which are part of a normal prod
      // configuration) we turn this flag off by default.
      '-vEval.EnableArgsInBacktraces=false',
      '-vEval.EnableIntrinsicsExtension=true',
      '-vEval.HHIRInliningIgnoreHints=false',
      '-vEval.HHIRAlwaysInterpIgnoreHint=false',
      '-vEval.FoldLazyClassKeys=false',
      '-vEval.EnableLogBridge=false',
      $mode,
      $options->wholecfg ? '-vEval.JitPGORegionSelector=wholecfg' : '',

      // load/store counters don't work on Ivy Bridge so disable for tests
      '-vEval.ProfileHWEnable=false',

      // use the temp path for embedded data
      '-vEval.EmbeddedDataExtractPath='.
      Status::getTestWorkingDir($test).
      escapeshellarg('/embedded_%{type}_%{buildid}'),

      // Stick to a single thread for retranslate-all
      '-vEval.JitWorkerThreads=1',
      '-vEval.JitWorkerThreadsForSerdes=1',
      '-vEval.AsyncJitWorkerThreads=1',
      '-vDebug.CoreDumpReportDirectory='.Status::getWorkingDir(),

      $options->args ?? '',
    ];

    if ($autoload_db_prefix is nonnull) {
      $args[] =
        '-vAutoload.DB.Path='.escapeshellarg("$autoload_db_prefix.$mode_num");
      if ($options->factsdir) {
        // enable LMDB facts by telling HHVM where to create facts LMDB instances
        $args[] =
          '-vAutoload.DB.LmdbDir='.escapeshellarg("$autoload_db_prefix.$mode_num");
      }
    }

    if ($options->retranslate_all is nonnull) {
      $args[] = '--count='.((int)$options->retranslate_all * 2);
      $args[] = '-vEval.JitPGO=true';
      $args[] = '-vEval.JitRetranslateAllRequest='.$options->retranslate_all;
      // Set to timeout.  We want requests to trigger retranslate all.
      $args[] = '-vEval.JitRetranslateAllSeconds=' . TIMEOUT_SECONDS;
      $args[] = '-vEval.JitWaitForAsyncJITBetweenRequests=1';
    }
    $args[] = '-vEval.EnableAsyncJITProfile=' . (($options->retranslate_all is nonnull && $options->async_jit_profile) ? '1' : '0');

    if ($options->recycle_tc is nonnull) {
      $args[] = '--count='.$options->recycle_tc;
      $args[] = '-vEval.StressUnitCacheFreq=1';
      $args[] = '-vEval.EnableReusableTC=true';
    }

    if ($options->jit_serialize is nonnull) {
      $args[] = '-vEval.JitPGO=true';
      $args[] = '-vEval.JitRetranslateAllRequest='.$options->jit_serialize;
      // Set to timeout.  We want requests to trigger retranslate all.
      $args[] = '-vEval.JitRetranslateAllSeconds=' . TIMEOUT_SECONDS;
    }

    if ($options->free_random_units is nonnull) {
      $args[] = '--count='.(int)$options->free_random_units;
      $args[] = '-vEval.FreeRandomUnits=1';
    }

    if ($options->repeat is nonnull) {
      $args[] = '--count='.(int)$options->repeat;
    }

    if ($options->hhas_round_trip) {
      $args[] = '-vEval.AllowHhas=1';
      $args[] = '-vEval.LoadFilepathFromUnitCache=1';
    }

    if (!$options->cores) {
      $args[] = '-vResourceLimit.CoreFileSize=0';
    }

    if ($options->dump_tc) {
      $args[] = '-vEval.DumpIR=2';
      $args[] = '-vEval.DumpTC=1';
    }

    if ($options->hh_single_type_check is nonnull) {
      $args[] = '--hh_single_type_check='.$options->hh_single_type_check;
    }

    if ($options->bespoke) {
      $args[] = '-vEval.BespokeArrayLikeMode=1';
      $args[] = '-vServer.APC.MemModelTreadmill=true';
    }

    // SBCC runtime args: inject Eval.SBCCPath when the test has SBCC metadata.
    // The .sbcc artifact was built by sbcc_mode_setup() before this function runs.
    if (sbcc_enabled($test)) {
      $args[] = '-vEval.SBCCPath='.escapeshellarg(sbcc_working_artifact($test));
    }

    $cmds[] = implode(' ', array_merge($args, $extra_args));
  }
  return $cmds;
}

function repo_separate(Options $options, string $test): bool {
  return $options->repo_separate &&
         !C\contains_key(read_test_config($test), 'hhbbc_opts');
}

// Return the command and the env to run it in.
function hhvm_cmd(
  Options $options,
  string $test,
  ?string $test_run = null,
  bool $is_temp_file = false
): (vec<string>, dict<string, mixed>) {
  $test_run ??= $test;
  // hdf support is only temporary until we fully migrate to ini
  // Discourage broad use.
  $hdf_suffix = ".use.for.ini.migration.testing.only.hdf";
  $hdf = file_exists($test.$hdf_suffix)
       ? '-c ' . $test . $hdf_suffix
       : "";

  $config_opts = read_dir_opts(dirname($test), 'opts');
  $file_opts = read_test_opts($test, 'opts', null, false);

  $input_dir = find_set_input_dir(dirname($test));
  if ($input_dir is nonnull) {
    $input_dir = realpath($input_dir);
  }

  $config_ini = find_file_for_dir(dirname($test), 'config.ini');
  invariant($config_ini is nonnull, "%s", __METHOD__);
  $cmds = hhvm_cmd_impl(
    $options,
    $test,
    $config_ini,
    Status::getTestWorkingDir($test) . '/autoloadDB',
    $hdf,
    find_debug_config($test, 'hphpd.ini'),
    $options->repo && $input_dir is nonnull
      ? '-vServer.SourceRoot='.$input_dir
      : '',
    $config_opts,
    $file_opts,
    $is_temp_file ? " --temp-file" : "",
    '--file',
    escapeshellarg(
      $options->repo && $input_dir is nonnull
        ? str_replace($input_dir.'/', '', realpath($test_run))
        : $test_run),

    // Dump profile to working dir
    '-vEval.JitSerializeDebugLocation='
      . Status::getTestWorkingDir($test) . '/debugdump.jit',
  );

  $cmd = "";

  if (read_test_flag($test, 'verify')) {
    $cmd .= " -m verify";
  }

  if ($options->cli_server) {
    $config = find_file_for_dir(dirname($test), 'config.ini');
    $servers = $options->servers as Servers;
    $server = $servers->configs[$config ?? ''];
    $socket = $server->cli_socket;
    $cmd .= ' -vEval.UseRemoteUnixServer=only';
    $cmd .= ' -vEval.UnixServerPath='.$socket;
    $cmd .= ' --count=3';
  }

  // Special support for tests that require a path to the current
  // test directory for things like prepend_file and append_file
  // testing.
  if (file_exists($test.'.ini')) {
    $contents = file_get_contents($test.'.ini');
    if (strpos($contents, '{PWD}') !== false) {
      $test_ini = Status::getTestWorkingDir($test) . '/config.ini';
      file_put_contents($test_ini,
                        str_replace('{PWD}', dirname($test), $contents));
      $cmd .= " -c $test_ini";
    }
  }
  if ($hdf !== "") {
    $contents = file_get_contents($test.$hdf_suffix);
    if (strpos($contents, '{PWD}') !== false) {
      $test_hdf = Status::getTestWorkingDir($test) . '/config' . $hdf_suffix;
      file_put_contents($test_hdf,
                        str_replace('{PWD}', dirname($test), $contents));
      $cmd .= " -c $test_hdf";
    }
  }

  if ($options->repo) {
    $repo_suffix = repo_separate($options, $test) ? 'hhbbc' : 'hhbc';

    $hhbbc_repo =
      "\"" . Status::getTestWorkingDir($test) . "/hhvm.$repo_suffix\"";
    $cmd .= ' -vRepo.Authoritative=true';
    $cmd .= " -vRepo.Path=$hhbbc_repo";

    $file_cache =
      "\"" . Status::getTestWorkingDir($test) . "/file.cache\"";
    $cmd .= " -vServer.FileCache=$file_cache";
  }

  if ($options->persistent) {
    $cmd .= ' -vEval.ForceAllPersistent=true';
  }

  if ($options->jitsample is nonnull) {
    $cmd .= ' -vDeploymentId="' . $options->jitsample . '"';
    $cmd .= ' --instance-id="' . $test . '"';
    $cmd .= ' -vEval.JitSampleRate=1';
    $cmd .= " -vScribe.Tables.hhvm_jit.include.*=instance_id";
    $cmd .= " -vScribe.Tables.hhvm_jit.include.*=deployment_id";
  }

  $env = \HH\FIXME\UNSAFE_CAST<dict<arraykey,mixed>,dict<string,mixed>>(
      \HH\global_get('_ENV') as dict<_, _>
  );
  $env['LC_ALL'] = 'C';
  $env['INPUTRC'] = test_dir().'/inputrc';

  // Apply the --env option.
  if ($options->env is nonnull) {
    foreach (explode(",", $options->env) as $arg) {
      $i = strpos($arg, '=');
      if ($i) {
        $key = substr($arg, 0, $i);
        $val = substr($arg, $i + 1);
        $env[$key] = $val;
      } else {
        unset($env[$arg]);
      }
    }
  }

  $env = get_test_env($options, $test, $env);

  $in = find_test_ext($test, 'in');
  if ($in is nonnull) {
    $cmd .= ' < ' . escapeshellarg($in);
    // If we're piping the input into the command then setup a simple
    // dumb terminal so hhvm doesn't try to control it and pollute the
    // output with control characters, which could change depending on
    // a wide variety of terminal settings.
    $env["TERM"] = "dumb";
  }

  $env['TMPDIR'] = Status::getTestTempDir($test);
  $env['HPHP_TEST_SOCKETDIR'] = Status::getSocketDir();
  if ($options->log) {
    $env['TRACE'] = 'printir:2';
    $env['HPHP_TRACE_FILE'] = $test . '.log';
  }

  if ($options->retranslate_all is nonnull ||
      $options->recycle_tc is nonnull ||
      $options->cli_server ||
      $options->free_random_units is nonnull ||
      $options->repeat is nonnull) {
    $env['HHVM_MULTI_COUNT_SEP'] = MULTI_REQUEST_SEP;
  }

  foreach ($cmds as $idx => $_) {
    $cmds[$idx] .= $cmd;
  }

  // Export SBCC working paths so PHP tests can locate runtime fixtures.
  if (sbcc_enabled($test)) {
    $run_root_file = Status::getTestWorkingDir($test).'/sbcc_run_root_path';
    $actual_run_root = is_file($run_root_file)
      ? trim(file_get_contents($run_root_file))
      : sbcc_working_run_root($test);
    $env['HPHP_TEST_SBCC_RUN_ROOT'] = $actual_run_root;
    $env['HPHP_TEST_SBCC_ARTIFACT'] = sbcc_working_artifact($test);
  }

  return tuple($cmds, $env);
}

function hphp_cmd(Options $options, string $test): string {
  // Transform extra_args like "-vName=Value" into "-vRuntime.Name=Value".
  $extra_args =
    preg_replace("/(^-v|\s+-v)\s*/", "$1Runtime.", $options->args ?? '');

  $compiler_args = extra_compiler_args($options);

  $hdf_suffix = ".use.for.ini.migration.testing.only.hdf";
  $hdf = file_exists($test.$hdf_suffix)
       ? '-c ' . $test . $hdf_suffix
       : "";

  if ($hdf !== "") {
    $contents = file_get_contents($test.$hdf_suffix);
    if (strpos($contents, '{PWD}') !== false) {
      $test_hdf = Status::getTestWorkingDir($test) . '/config' . $hdf_suffix;
      file_put_contents($test_hdf,
                        str_replace('{PWD}', dirname($test), $contents));
      $hdf = " -c $test_hdf";
    }
  }

  $config_hphp_opts = read_dir_opts(dirname($test), 'hphp_opts');

  $input_dir = find_set_input_dir(dirname($test));
  if ($input_dir is nonnull) {
    $input_dir = realpath($input_dir);
  }

  return implode(" ", vec[
    hphpc_path($options),
    '--hphp',
    '-vUseHHBBC='. (repo_separate($options, $test) ? 'false' : 'true'),
    '--config',
    find_test_ext($test, 'ini', 'hphp_config'),
    $hdf,
    $input_dir is nonnull ? '--input-dir='.$input_dir : '',
    $input_dir is nonnull ? '-vRuntime.Server.SourceRoot='.$input_dir : '',
    $options->cores ? '' : '-vRuntime.ResourceLimit.CoreFileSize=0',
    '-vRuntime.Debug.CoreDumpReportDirectory='.Status::getWorkingDir(),
    '-vRuntime.Eval.EnableIntrinsicsExtension=true',
    // EnableArgsInBacktraces disables most of HHBBC's DCE optimizations.
    // In order to test those optimizations (which are part of a normal prod
    // configuration) we turn this flag off by default.
    '-vRuntime.Eval.EnableArgsInBacktraces=false',
    '-vRuntime.Eval.FoldLazyClassKeys=false',
    '-vRuntime.Eval.EnableLogBridge=false',
    '-vCachePHPFile=true',
    '-vParserThreadCount=' . ($options->repo_threads ?? 1),
    '-l1',
    '-o "' . Status::getTestWorkingDir($test) . '"',
    '--file-cache="'.Status::getTestWorkingDir($test).'/file.cache"',
    $input_dir is nonnull
      ? str_replace($input_dir.'/', '', realpath($test))
      : "\"$test\"",
    "-vExternWorker.WorkingDir=".Status::getTestWorkingDir($test),
    $config_hphp_opts,
    $extra_args,
    $compiler_args,
    read_test_opts($test, 'hphp_opts', $input_dir),
  ]);
}

function hphpc_path(Options $options): string {
  return hhvm_path();
}

function hhbbc_cmd(Options $options, string $test): string {
  $working_dir = Status::getTestWorkingDir($test);
  return implode(" ", vec[
    hphpc_path($options),
    '--hhbbc',
    '--no-logging',
    $options->cores ? '' : '--no-cores',
    '--parallel-num-threads=' . ($options->repo_threads ?? 1),
    '--parallel-final-threads=' . ($options->repo_threads ?? 1),
    '--extern-worker-working-dir=' . $working_dir,
    read_test_opts($test, 'hhbbc_opts', null, false),
    "-o \"$working_dir/hhvm.hhbbc\" \"$working_dir/hhvm.hhbc\"",
  ]);
}

// Split the output into everything before a stack trace dump and
// everything after (including the dump).
function split_stack_trace_from_output(string $str): (string, string) {
  $before = vec[];
  $after = vec[];
  $found = false;
  // Any of the below strings indicate the beginning of a crash
  // report.
  foreach (explode("\n", $str) as $line) {
    if (!$found) {
      if (stripos($line, "Core dumped") === 0 ||
          stripos($line, "Stack trace in") === 0 ||
          stripos($line, "Assertion Failure") !== false) {
        $found = true;
      }
    }
    if ($found) {
      $after[] = $line;
    } else {
      $before[] = $line;
    }
  }
  // Only provide the last 200 lines of before the dump.
  return tuple(
    implode("\n", array_slice($before, -200)),
    implode("\n", $after)
  );
}

// Run the specified command with a TIMEOUT_SECONDS timeout. If the
// command is successful, return a tuple containing the command's
// output and true. If unsuccessful, return a tuple containing an
// error message containing useful information and false. A command is
// judged to fail if it returns a non-successful error code, writes to
// stderr (we redirect stderr to stdout), or times out.
function exec_with_timeout(string $cmd,
                           ?dict<string, mixed> $env = null,
                           ?keyset<int> $success_codes = null): (string, bool) {
  if ($success_codes is null) $success_codes = keyset[0];

  $pipes = null;
  // Use exec so that the shell execs the command directly instead of
  // forking. Redirect stderr to stdout because the tests will write
  // to both.
  $proc = proc_open(
    'exec ' . $cmd . ' 2>&1',
    dict[0 => vec['pipe', 'r'],
         1 => vec['pipe', 'w'],
         2 => vec['pipe', 'w']],
    inout $pipes,
    null,
    $env
  );
  $pipes as nonnull;
  fclose($pipes[0]);

  stream_set_blocking($pipes[1], false);
  stream_set_blocking($pipes[2], false);

  $stdout = '';
  $stderr = '';

  // Loop, reading stdout and stderr until both stdout and stderr are
  // closed, the process terminates, or we timeout.
  $end = mtime() + TIMEOUT_SECONDS;
  $status = proc_get_status($proc);
  while (true) {
    $now = mtime();
    if ($now >= $end) break;

    $read = vec[];
    if (!feof($pipes[1])) $read[] = $pipes[1];
    if (!feof($pipes[2])) $read[] = $pipes[2];
    if (count($read) === 0) break;

    $write = null;
    $except = null;

    // Don't wait longer than 5 seconds at a time so we can poll for
    // the process exiting without closing stdout/stderr. This can
    // happen if the process forks (the forked child can survive and
    // keep the output open). If the process has already exited, don't
    // sleep at all, because we're trying to drain all output.
    $timeout = (int)min(
      ($end - $now) * 1000000,
      $status['running'] ? 5000000 : 0
    );
    $available = stream_select(
      inout $read,
      inout $write,
      inout $except,
      0,
      $timeout
    );
    if ($available === false) continue;
    if ($available === 0) {
      if ($status['running']) $status = proc_get_status($proc);
      if (!$status['running']) break;
      continue;
    }

    $read as nonnull;
    foreach ($read as $pipe) {
      $t = fread($pipe, 8*1024);
      if ($t === false) continue;
      if ($pipe === $pipes[1]) {
        $stdout .= $t;
      } else if ($pipe === $pipes[2]) {
        $stderr .= $t;
      }
    }
  }
  fclose($pipes[1]);
  fclose($pipes[2]);

  $timedout = false;
  while ($status['running']) {
    $status = proc_get_status($proc);
    if (!$status['running']) break;
    $now = mtime();
    if ($now >= $end) {
      $timedout = true;
      $ignore1 = '';
      $ignore2 = 0;
      // We've timed out. Kill the command and any children it might
      // have created.
      exec(
        'pkill --signal 9 -P ' . $status['pid'] . ' 2> /dev/null',
        inout $ignore1,
        inout $ignore2
      );
      posix_kill($status['pid'], SIGKILL);
    }
    // Sleep then loop until the process actually terminates.
    usleep(500000);
  }

  $exit_code = $status['exitcode'];
  proc_close($proc);

  if (!$timedout && C\contains($success_codes, $exit_code) && Str\is_empty($stderr)) {
    return tuple($stdout, true);
  }

  // The command failed. Construct a useful error message.
  $error = "Running '$cmd' failed ";
  if ($timedout) {
    $error .= ' (timed out and was killed)';
  } else {
    $error .= " (exit-code $exit_code)";
  }
  if (!Str\is_empty($stderr)) $error .= ' (wrote to stderr)';

  list($before_dump, $dump) = split_stack_trace_from_output($stdout);

  $pid = $status['pid'];
  $stack_file = Status::getWorkingDir(). "/stacktrace.$pid.log";

  $stack = null;
  if (file_exists($stack_file)) $stack = file_get_contents($stack_file);

  if ($stack is nonnull && HH\legacy_is_truthy($stack)) {
    $error .= "\nstdout:\n$before_dump\nstderr:\n$stderr";
    $error .= "\nContents of $stack_file:\n$stack";
  } else if (!Str\is_empty($dump)) {
    $error .= "\nstdout:\n$dump\nstderr:\n$stderr";
  } else {
    $error .= "\nstdout:\n$before_dump\nstderr:\n$stderr";
  }
  return tuple($error, false);
}

function repo_mode_compile(Options $options, string $test): (string, bool) {
  $hphp = hphp_cmd($options, $test);
  list($output, $success) = exec_with_timeout($hphp);
  if ($success && repo_separate($options, $test)) {
    $hhbbc = hhbbc_cmd($options, $test);
    list($separate_output, $success) = exec_with_timeout($hhbbc);
    $output = $output . $separate_output;
  }
  return tuple($output, $success);
}


// Minimal support for sending messages between processes over named pipes.
//
// Non-buffered pipe writes of up to 512 bytes (PIPE_BUF) are atomic.
//
// Packet format:
//   8 byte zero-padded hex pid
//   4 byte zero-padded hex type
//   4 byte zero-padded hex body size
//   N byte string body
//
// The first call to "getInput()" or "getOutput()" in any process will
// block until some other process calls the other method.
//
class Queue {
  // The path to the FIFO, until destroyed.
  private ?string $path = null;

  private ?resource $input = null;
  private ?resource $output = null;

  // Pipes writes are atomic up to 512 bytes (up to 4096 bytes on linux),
  // and we use a 16 byte header, leaving this many bytes available for
  // each chunk of "body" (see "$partials").
  const int CHUNK = 512 - 16;

  // If a message "body" is larger than CHUNK bytes, then writers must break
  // it into chunks, and send all but the last chunk with type 0.  The reader
  // collects those chunks in this Map (indexed by pid), until the final chunk
  // is received, and the chunks can be reassembled.
  private Map<int, Vector<string>> $partials = Map {};

  public function __construct(string $dir): void {
    $path = \tempnam($dir, "queue.mkfifo.");
    \unlink($path);
    if (!\posix_mkfifo($path, 0700)) {
      // Only certain directories support "posix_mkfifo()".
      throw new \Exception("Failed to create FIFO at '$path'");
    }
    $this->path = $path;
  }

  private function getInput(): resource {
    $input = $this->input;
    if ($input is null) {
      $path = $this->path;
      if ($path is null) {
        throw new \Exception("Missing FIFO path");
      }
      $input = \fopen($path, "r");
      $this->input = $input;
    }
    return $input;
  }

  private function getOutput(): resource {
    $output = $this->output;
    if ($output is null) {
      $path = $this->path;
      if ($path is null) {
        throw new \Exception("Missing FIFO path");
      }
      $output = \fopen($path, "a");
      $this->output = $output;
    }
    return $output;
  }

  private function validate(int $pid, int $type, int $blen): void {
    if ($pid < 0 || $pid >= (1 << 22)) {
      throw new \Exception("Illegal pid $pid");
    }
    if ($type < 0 || $type >= 0x10000) {
      throw new \Exception("Illegal type $type");
    }
    if ($blen < 0 || $blen > static::CHUNK) {
      throw new \Exception("Illegal blen $blen");
    }
  }

  // Read one packet header or body.
  private function read(int $n): string {
    $input = $this->getInput();
    $result = "";
    while (\strlen($result) < $n) {
      $r = fread($input, $n - \strlen($result));
      if ($r is string) {
        $result .= $r;
      } else {
        throw new \Exception("Failed to read $n bytes");
      }
    }
    return $result;
  }

  // Receive one raw message (pid, type, body).
  public function receive(): (int, int, string) {
    $type = null;
    $body = "";
    while (true) {
      $header = $this->read(16);
      $pid = intval(substr($header, 0, 8) as string, 16);
      $type = intval(substr($header, 8, 4) as string, 16);
      $blen = intval(substr($header, 12, 4) as string, 16);
      $this->validate($pid, $type, $blen);
      $body = $this->read($blen);
      if ($type === 0) {
        $this->partials[$pid] ??= Vector {};
        $this->partials[$pid][] = $body;
      } else {
        $chunks = $this->partials[$pid] ?? null;
        if ($chunks is nonnull) {
          $chunks[] = $body;
          $body = \implode("", $chunks);
          $this->partials->removeKey($pid);
        }
        return tuple($pid, $type, $body);
      }
    }
  }

  // Receive one message (pid, type, message).
  // Note that the raw body is processed using "unserialize()".
  public function receiveMessage(): (int, int, ?Message) {
    list($pid, $type, $body) = $this->receive();
    $msg = unserialize($body) as ?Message;
    return tuple($pid, $type, $msg);
  }

  private function write(int $pid, int $type, string $body): void {
    $output = $this->getOutput();
    $blen = \strlen($body);
    $this->validate($pid, $type, $blen);
    $packet = sprintf("%08x%04x%04x%s", $pid, $type, $blen, $body);
    $n = \strlen($packet);
    if ($n !== 16 + $blen) {
      throw new \Exception("Illegal packet");
    }
    // Hack's "fwrite()" is never buffered, which is especially
    // critical for pipe writes, to ensure that they are actually atomic.
    // See the documentation for "PlainFile::writeImpl()".  But just in
    // case, we add an explicit "fflush()" below.
    $bytes_out = fwrite($output, $packet, $n);
    if ($bytes_out !== $n) {
      throw new \Exception(
        "Failed to write $n bytes; only $bytes_out were written"
      );
    }
    fflush($output);
  }

  // Send one serialized message.
  public function send(int $type, string $body): void {
    $pid = \posix_getpid();
    $blen = \strlen($body);
    $chunk = static::CHUNK;
    if ($blen > $chunk) {
      for ($i = 0; $i + $chunk < $blen; $i += $chunk) {
        $this->write($pid, 0, \substr($body, $i, $chunk) as string);
      }
      $this->write($pid, $type, \substr($body, $i) as string);
    } else {
      $this->write($pid, $type, $body);
    }
  }

  // Send one message after serializing it.
  public function sendMessage(int $type, ?Message $msg): void {
    $body = serialize($msg);
    $this->send($type, $body);
  }

  public function destroy(): void {
    if ($this->input is nonnull) {
      fclose($this->input);
      $this->input = null;
    }
    if ($this->output is nonnull) {
      fclose($this->output);
      $this->output = null;
    }
    if ($this->path is nonnull) {
      \unlink($this->path);
      $this->path = null;
    }
  }
}

final class Message {
  public function __construct(
    public string $test,
    public float $time,
    public int $stime,
    public int $etime,
    public ?string $reason = null,
  ) {
  }
}

enum TempDirRemove: int {
  ALWAYS = 0;
  ON_RUN_SUCCESS = 1;
  NEVER = 2;
}

type TestResult = shape(
  'name' => string,
  'status' => string,
  'start_time' => int,
  'end_time' => int,
  'time' => float,
  ?'details' => string,
);

final class Status {
  private static vec<TestResult> $results = vec[];
  private static int $mode = 0;

  private static bool $use_color = false;

  public static bool $nofork = false;
  private static ?Queue $queue = null;
  private static bool $killed = false;
  public static TempDirRemove $temp_dir_remove = TempDirRemove::ALWAYS;
  private static int $return_value = 255;

  private static float $overall_start_time = 0.0;
  private static float $overall_end_time = 0.0;

  private static string $workdir = "";
  private static string $sockdir = "";

  public static vec<string> $tests = vec[];

  public static int $passed = 0;
  public static int $skipped = 0;
  public static dict<string, int> $skip_reasons = dict[];
  public static int $failed = 0;

  public static keyset<int> $children = keyset[];
  public static int $printer_pid = -1;

  const int MODE_NORMAL = 0;
  const int MODE_VERBOSE = 1;
  const int MODE_TESTPILOT = 3;
  const int MODE_RECORD_FAILURES = 4;

  const int MSG_STARTED = 7;
  const int MSG_FINISHED = 1;
  const int MSG_TEST_PASS = 2;
  const int MSG_TEST_FAIL = 4;
  const int MSG_TEST_SKIP = 5;
  const int MSG_SERVER_RESTARTED = 6;

  const int RED = 31;
  const int GREEN = 32;
  const int YELLOW = 33;
  const int BLUE = 34;

  public static function createWorkingDir(?string $working_dir): void {
    $parent = $working_dir ?? sys_get_temp_dir();
    if (substr($parent, -1) !== "/") $parent .= "/";
    self::$workdir = HH\Lib\_Private\_OS\mkdtemp($parent . 'hphp-test-XXXXXX');
    self::$sockdir = self::$workdir . '/sock';
    mkdir(self::$sockdir, 0777, false);
    file_put_contents(self::$workdir . '/work-queue', '0');
  }

  public static function getWorkingDir(): string {
    return self::$workdir;
  }

  // Paths for Unix sockets have size limitations, so provide a
  // separate shorter path to put them in.
  public static function getSocketDir(): string {
    return self::$sockdir;
  }

  public static function createTestWorkingDir(string $test): void {
    $path = self::getTestWorkingDir($test);
    mkdir($path, 0777, true);
    mkdir($path . '/temp', 0777, false);
  }

  public static function getTestWorkingDir(string $test): string {
    return self::$workdir . '/' . $test;
  }

  public static function getTestTempDir(string $test): string {
    return self::getTestWorkingDir($test) . '/temp';
  }

  public static function createServerWorkingDir(): string {
    return HH\Lib\_Private\_OS\mkdtemp(self::$workdir . '/server-XXXXXX');
  }

  public static function writeDiff(string $test, string $diff): void {
    $path = self::getTestWorkingDir($test) . '/diff';
    file_put_contents($path, $diff);
  }

  public static function diffForTest(string $test): string {
    $path = self::getTestWorkingDir($test) . '/diff';
    $diff = file_get_contents($path);
    return $diff === false ? '' : $diff;
  }

  public static function cleanupTestWorkingDir(string $test): void {
    self::removeDirectory(self::getTestWorkingDir($test));
  }

  private static function cleanupWorkingDir(): void {
    self::removeDirectory(self::$workdir);
  }

  private static function cleanupEmptyWorkingDir(): void {
    self::removeEmptyDirectories(self::$workdir);
  }

  private static function removeDirectory(string $dir): void {
    $files = scandir($dir);
    if ($files === false) return;
    foreach ($files as $file) {
      if ($file === '.' || $file === '..') continue;
      $path = $dir . "/" . $file;
      if (is_dir($path)) {
        self::removeDirectory($path);
      } else {
        unlink($path);
      }
    }
    rmdir($dir);
  }

  // This is similar to removeDirectory but it only removes empty
  // directores. This allows us to clean up paths like test/quick/vec
  // in our run's temporary directory if all the tests in them passed,
  // but it leaves test workdirs of failed tests (that we didn't
  // remove with clean_test_files because the test failed) and
  // directores under them alone even if they're empty.
  private static function removeEmptyDirectories(string $dir): bool {
    $is_now_empty = true;
    $files = scandir($dir);
    foreach ($files as $file) {
      if ($file === '.' || $file === '..') continue;
      $path = $dir . "/" . $file;
      if (!is_dir($path)) {
        $is_now_empty = false;
        continue;
      }
      if (self::removeEmptyDirectories($path)) {
        rmdir($path);
      } else {
        $is_now_empty = false;
      }
    }
    return $is_now_empty;
  }

  public static function setMode(int $mode): void {
    self::$mode = $mode;
  }

  public static function getMode(): int {
    return self::$mode;
  }

  public static function setUseColor(bool $use): void {
    self::$use_color = $use;
  }

  public static function addTestTimesSerial(
    dict<string, TestResult> $results,
  ): float {
    $time = 0.0;
    foreach ($results as $result) {
      $time += $result['time'];
    }
    return $time;
  }

  public static function getOverallStartTime(): float {
    return self::$overall_start_time;
  }

  public static function getOverallEndTime(): float {
    return self::$overall_end_time;
  }

  public static function started(): void {
    self::send(self::MSG_STARTED, null);
    self::$overall_start_time = mtime();
  }

  public static function finished(int $return_value): void {
    self::$overall_end_time = mtime();
    self::$return_value = $return_value;
    self::send(self::MSG_FINISHED, null);
  }

  public static function destroy(): void {
    if (!self::$killed) {
      self::$killed = true;
      if (self::$queue is nonnull) {
        self::$queue->destroy();
        self::$queue = null;
      }
      // Kill any pending children (including the printer if it's still
      // running). The most common case of this is if `error()` is called from
      // the main loop.
      $children = self::$children;
      if (self::$printer_pid > 0) {
        $children[] = self::$printer_pid;
      }
      foreach (self::$children as $child) {
        if ($child > 1) {
          // No reason to think that SIGKILL is necessary.
          posix_kill($child, SIGINT);
        }
      }
      switch (self::$temp_dir_remove) {
        case TempDirRemove::NEVER:
          break;
        case TempDirRemove::ON_RUN_SUCCESS:
          if (self::$return_value !== 0) {
            self::cleanupEmptyWorkingDir();
            break;
          }
          self::cleanupWorkingDir();
          break;
        case TempDirRemove::ALWAYS:
          self::cleanupWorkingDir();
          break;
      }
    }
  }

  public static function destroyFromSignal(int $_signo): void {
    self::destroy();
  }

  public static function registerCleanup(bool $no_clean): void {
    if (self::getMode() === self::MODE_TESTPILOT ||
        self::getMode() === self::MODE_RECORD_FAILURES) {
      self::$temp_dir_remove = TempDirRemove::ALWAYS;
    } else if ($no_clean) {
      self::$temp_dir_remove = TempDirRemove::NEVER;
    } else {
      self::$temp_dir_remove = TempDirRemove::ON_RUN_SUCCESS;
    }
    register_shutdown_function(self::destroy<>);
    pcntl_signal(SIGTERM, self::destroyFromSignal<>);
    pcntl_signal(SIGINT, self::destroyFromSignal<>);
  }

  public static function serverRestarted(): void {
    self::send(self::MSG_SERVER_RESTARTED, null);
  }

  public static function pass(
    string $test, float $time, int $stime, int $etime,
  ): void {
    self::$results[] = shape(
      'name' => $test,
      'status' => 'passed',
      'start_time' => $stime,
      'end_time' => $etime,
      'time' => $time
    );
    self::send(
      self::MSG_TEST_PASS,
      new Message($test, $time, $stime, $etime),
    );
  }

  public static function skip(
    string $test, string $reason, float $time, int $stime, int $etime,
  ): void {
    self::$results[] = shape(
      'name' => $test,
      /* testpilot needs a positive response for every test run, report
       * that this test isn't relevant so it can silently drop. */
      'status' => self::getMode() === self::MODE_TESTPILOT
        ? 'not_relevant'
        : 'skipped',
      'start_time' => $stime,
      'end_time' => $etime,
      'time' => $time,
    );
    self::send(
      self::MSG_TEST_SKIP,
      new Message($test, $time, $stime, $etime, $reason),
    );
  }

  public static function fail(
    string $test, float $time, int $stime, int $etime, string $diff,
  ): void {
    self::$results[] = shape(
      'name' => $test,
      'status' => 'failed',
      'start_time' => $stime,
      'end_time' => $etime,
      'time' => $time,
      'details' => self::utf8Sanitize($diff),
    );
    self::send(
      self::MSG_TEST_FAIL,
      new Message($test, $time, $stime, $etime),
    );
  }

  public static function handle_message(int $type, ?Message $message): bool {
    switch ($type) {
      case Status::MSG_STARTED:
        break;

      case Status::MSG_FINISHED:
        return false;

      case Status::MSG_SERVER_RESTARTED:
        /* HH_FIXME[4489]: Revealed through strict switches */
        switch (Status::getMode()) {
          case Status::MODE_NORMAL:
            if (!Status::hasCursorControl()) {
              Status::sayColor(Status::RED, 'x');
            }
            break;

          case Status::MODE_VERBOSE:
            Status::sayColor(
              Status::YELLOW,
              "failed to talk to server\n"
            );
            break;

          case Status::MODE_TESTPILOT:
            break;

          case Status::MODE_RECORD_FAILURES:
            break;
        }
        break;

      case Status::MSG_TEST_PASS:
        self::$passed++;
        invariant($message is nonnull, "%s", __METHOD__);
        /* HH_FIXME[4489]: Revealed through strict switches */
        switch (Status::getMode()) {
          case Status::MODE_NORMAL:
            if (!Status::hasCursorControl()) {
              Status::sayColor(Status::GREEN, '.');
            }
            break;

          case Status::MODE_VERBOSE:
            Status::sayColor(
              $message->test." ",
              Status::GREEN,
              sprintf("passed (%.2fs)\n", $message->time),
            );
            break;

          case Status::MODE_TESTPILOT:
            Status::sayTestpilot(
              $message->test,
              'passed',
              $message->stime,
              $message->etime,
              $message->time,
            );
            break;

          case Status::MODE_RECORD_FAILURES:
            break;
        }
        break;

      case Status::MSG_TEST_SKIP:
        self::$skipped++;
        invariant($message is nonnull, "%s", __METHOD__);
        $reason = $message->reason;
        invariant($reason is nonnull, "%s", __METHOD__);
        self::$skip_reasons[$reason] ??= 0;
        self::$skip_reasons[$reason]++;

        /* HH_FIXME[4489]: Revealed through strict switches */
        switch (Status::getMode()) {
          case Status::MODE_NORMAL:
            if (!Status::hasCursorControl()) {
              Status::sayColor(Status::YELLOW, 's');
            }
            break;

          case Status::MODE_VERBOSE:
            Status::sayColor($message->test." ", Status::YELLOW, "skipped");
            Status::sayColor(" - reason: $reason");
            Status::sayColor(sprintf(" (%.2fs)\n", $message->time));
            break;

          case Status::MODE_TESTPILOT:
            $skip_msg = $message->test." skipped (by run.php)";
            $skip_msg .= " - reason: $reason";
            Status::sayTestpilot(
              $message->test,
              'not_relevant',
              $message->stime,
              $message->etime,
              $message->time,
              $skip_msg,
            );
            break;

          case Status::MODE_RECORD_FAILURES:
            break;
        }
        break;

      case Status::MSG_TEST_FAIL:
        self::$failed++;
        invariant($message is nonnull, "%s", __METHOD__);
        /* HH_FIXME[4489]: Revealed through strict switches */
        switch (Status::getMode()) {
          case Status::MODE_NORMAL:
            if (Status::hasCursorControl()) {
              print "\033[2K\033[1G";
            }
            $diff = Status::diffForTest($message->test);
            $test = $message->test;
            Status::sayColor(
              Status::RED,
              "\nFAILED: $test\n$diff\n",
            );
            break;

          case Status::MODE_VERBOSE:
            Status::sayColor(
              $message->test." ",
              Status::RED,
              sprintf("FAILED (%.2fs)\n", $message->time),
            );
            break;

          case Status::MODE_TESTPILOT:
            Status::sayTestpilot(
              $message->test,
              'failed',
              $message->stime,
              $message->etime,
              $message->time,
            );
            break;

          case Status::MODE_RECORD_FAILURES:
            break;
        }
        break;

      default:
        error("Unknown message $type");
    }
    return true;
  }

  private static function send(int $type, ?Message $msg): void {
    if (self::$killed) {
      return;
    }
    if (self::$nofork) {
      self::handle_message($type, $msg);
      return;
    }
    self::getQueue()->sendMessage($type, $msg);
  }

  /**
    * Takes a variable number of string or int arguments. If color output is
    * enabled and any one of the arguments is preceded by an integer (see the
    * color constants above), that argument will be given the indicated color.
   */
  public static function sayColor(arraykey ...$args): void {
    $n = count($args);
    for ($i = 0; $i < $n;) {
      $arg = $args[$i];
      $i++;
      if ($arg is int) {
        $color = $arg;
        if (self::$use_color) {
          print "\033[0;{$color}m";
        }
        $arg = $args[$i];
        $i++;
        print $arg;
        if (self::$use_color) {
          print "\033[0m";
        }
      } else {
        print $arg;
      }
    }
  }

  public static function sayTestpilot(
      string $test, string $status, int $stime, int $etime, float $time,
      ?string $msg = null,
  ): void {
    $start = dict['op' => 'start', 'test' => $test];
    $end = dict['op' => 'test_done', 'test' => $test, 'status' => $status,
                 'start_time' => $stime, 'end_time' => $etime, 'time' => $time];
    if ($status === 'failed') {
      $end['details'] = self::utf8Sanitize(Status::diffForTest($test));
    } else if ($msg is nonnull) {
      $end['details'] = self::utf8Sanitize($msg);
    }
    self::say($start, $end);
  }

  public static function getResults(): vec<TestResult> {
    return self::$results;
  }

  /** Output is in the format expected by JsonTestRunner. */
  public static function say(dict<string, mixed> ...$args): void {
    $data = array_map(
      $row ==> self::jsonEncode($row) . "\n",
      $args
    );
    fwrite(HH\stderr(), implode("", $data));
  }

  <<__Memoize>>
  public static function hasCursorControl(): bool {
    // for runs on hudson-ci.org (aka jenkins).
    if (getenv("HUDSON_URL")) {
      return false;
    }
    // for runs on travis-ci.org
    if (getenv("TRAVIS")) {
      return false;
    }
    $stty = self::getSTTY();
    if (Str\is_empty($stty)) {
      return false;
    }
    // emacs hack-mode really doesn't like <undef> inside a string...
    return strpos($stty, 'erase = ' . '<' . 'undef' . '>') === false;
  }

  <<__Memoize>>
  public static function getSTTY(): string {
    $descriptorspec = dict[1 => vec["pipe", "w"], 2 => vec["pipe", "w"]];
    $pipes = null;
    $process = proc_open(
      'stty -a', $descriptorspec, inout $pipes, null, null,
      dict['suppress_errors' => true]
    );
    $pipes as nonnull;
    $stty = stream_get_contents($pipes[1]);
    proc_close($process);
    return $stty;
  }

  public static function utf8Sanitize(string $str): string {
    return UConverter::transcode($str, 'UTF-8', 'UTF-8');
  }

  public static function jsonEncode(mixed $data): string {
    return json_encode($data, JSON_UNESCAPED_SLASHES);
  }

  public static function getQueue(): Queue {
    if (self::$queue is null) {
      if (self::$killed) error("Killed!");
      self::$queue = new Queue(self::$workdir);
    }
    return self::$queue;
  }

  private static function nextTestIndex(): int {
    $path = self::$workdir . '/work-queue';
    $file = fopen($path, "r+");
    try {
      $would_block = false;
      if (!flock($file, LOCK_EX, inout $would_block)) {
        invariant_violation("unable to lock %s", $path);
      }
      $str = fread($file, 16);
      if ($str === false) {
        invariant_violation("unable to read from %s", $path);
      }
      $idx = Str\to_int($str);
      if ($idx is null) {
        invariant_violation("read non-integer \"%s\" from %s", $str, $path);
      }
      if (fseek($file, 0) < 0) {
        invariant_violation("unable to seek to beginning of %s", $path);
      }
      fprintf($file, "%s", $idx + 1);
      fflush($file);
      return $idx;
    } finally {
      if ($file is resource) {
        fclose($file);
      }
    }
  }

  public static function nextTest(): ?string {
    $idx = self::nextTestIndex();
    if ($idx >= count(self::$tests)) return null;
    return self::$tests[$idx];
  }
}

function clean_test_files(string $test, Options $options): void {
  if ($options->no_clean) return;
  Status::cleanupTestWorkingDir($test);
}

function child_main(
  Options $options,
  vec<string> $serial_tests,
  string $json_results_file,
): int {
  foreach ($serial_tests as $test) {
    run_and_log_test($options, $test);
  }
  while (true) {
    $test = Status::nextTest();
    if ($test is null) break;
    run_and_log_test($options, $test);
  }
  $results = Status::getResults();
  file_put_contents($json_results_file, json_encode($results));
  foreach ($results as $result) {
    if ($result['status'] === 'failed') {
      return 1;
    }
  }
  return 0;
}

/**
 * The runif feature is similar in spirit to skipif, but instead of allowing
 * one to run arbitrary code it can only skip based on pre-defined reasons
 * understood by the test runner.
 *
 * The .runif file should consist of one or more lines made up of words
 * separated by spaces, optionally followed by a comment starting with #.
 * Empty lines (or lines with only comments) are ignored. The first word
 * determines the interpretation of the rest. The .runif file will allow the
 * test to run if all the non-empty lines 'match'.
 *
 * Currently supported operations:
 *   os [not] <os_name> # matches if we are (or are not) on the named OS
 *   file <path> # matches if the file at the (possibly relative) path exists
 *   euid [not] root # matches if we are (or are not) running as root (euid==0)
 *   extension <extension_name> # matches if the named extension is available
 *   function <function_name> # matches if the named function is available
 *   class <class_name> # matches if the named class is available
 *   method <class_name> <method name> # matches if the method is available
 *   const <constant_name> # matches if the named constant is available
 *   # matches if any named locale is available for the named LC_* category
 *   locale LC_<something> <locale name>[ <another locale name>]
 *
 * Several functions in this implementation return RunifResult. Valid sets of
 * keys are:
 *   valid, error # valid will be false
 *   valid, match # valid will be true, match will be true
 *   valid, match, skip_reason # valid will be true, match will be false
 */
type RunifResult = shape(
  'valid' => bool, // was the runif file valid
  ?'error' => string, // if !valid, what was the problem
  ?'match' => bool, // did the line match/did all the lines in the file match
  ?'skip_reason' => string, // if !match, the skip reason to use
);

<<__Memoize>>
function runif_canonical_os(): string {
  if (PHP_OS === 'Linux' || PHP_OS === 'Darwin') return PHP_OS;
  if (substr(PHP_OS, 0, 3) === 'WIN') return 'WIN';
  invariant_violation('add proper canonicalization for your OS');
}

function runif_known_os(string $match_os): bool {
  switch ($match_os) {
    case 'Linux':
    case 'Darwin':
    case 'WIN':
      return true;
    default:
      return false;
  }
}

function runif_os_matches(vec<string> $words): RunifResult {
  if (count($words) === 2) {
    if ($words[0] !== 'not') {
      return shape('valid' => false, 'error' => "malformed 'os' match");
    }
    $match_os = $words[1];
    $invert = true;
  } else if (count($words) === 1) {
    $match_os = $words[0];
    $invert = false;
  } else {
    return shape('valid' => false, 'error' => "malformed 'os' match");
  }
  if (!runif_known_os($match_os)) {
    return shape('valid' => false, 'error' => "unknown os '$match_os'");
  }
  $matches = (runif_canonical_os() === $match_os);
  if ($matches !== $invert) return shape('valid' => true, 'match' => true);
  return shape(
    'valid' => true,
    'match' => false,
    'skip_reason' => 'skip-runif-os-' . implode('-', $words)
  );
}

function runif_file_matches(vec<string> $words): RunifResult {
  /* This implementation has a trade-off. On the one hand, we could get more
   * accurate results if we do the check in a process with the same configs as
   * the test via runif_test_for_feature (e.g. if config differences make a
   * file we can see invisible to the test). However, this check was added to
   * skip tests where the test configs depend on a file that may be absent, in
   * which case hhvm configured the same way as the test cannot run. By doing
   * the check ourselves we can successfully skip such tests.
   */
  if (count($words) !== 1) {
    return shape('valid' => false, 'error' => "malformed 'file' match");
  }
  if (file_exists($words[0])) {
    return shape('valid' => true, 'match' => true);
  }
  return shape(
    'valid' => true,
    'match' => false,
    'skip_reason' => 'skip-runif-file',
  );
}

function runif_test_for_feature(
  Options $options,
  string $test,
  string $bool_expression,
): bool {
  $tmp = tempnam(Status::getWorkingDir(), 'test-run-runif-');
  file_put_contents(
    $tmp,
    "<?hh\n" .
      "<<__EntryPoint>> function main(): void {\n" .
      "  echo ($bool_expression) as bool ? 'PRESENT' : 'ABSENT';\n" .
      "}\n",
  );

  // Run the check in non-repo mode to avoid building the repo (same features
  // should be available). Pick the mode arbitrarily for the same reason.
  $options_without_repo = clone $options;
  $options_without_repo->repo = false;
  list($hhvm, $_) = hhvm_cmd($options_without_repo, $test, $tmp, true);
  $hhvm = $hhvm[0];
  // Remove any --count <n> from the command
  $hhvm = preg_replace('/ --count[ =]\d+/', '', $hhvm);
  // some tests set open_basedir to a restrictive value, override to permissive
  $hhvm .= ' -dopen_basedir= ';

  $result = shell_exec("$hhvm 2>&1");
  invariant ($result !== false, 'shell_exec in runif_test_for_feature failed');
  $result = trim($result);
  if ($result === 'ABSENT') return false;
  if ($result === 'PRESENT') return true;
  invariant_violation(
    "unexpected output from shell_exec in runif_test_for_feature: '%s'",
    $result
  );
}

function runif_euid_matches(
  Options $options,
  string $test,
  vec<string> $words,
): RunifResult {
  if (count($words) === 2) {
    if ($words[0] !== 'not' || $words[1] !== 'root') {
      return shape('valid' => false, 'error' => "malformed 'euid' match");
    }
    $invert = true;
  } else if (count($words) === 1) {
    if ($words[0] !== 'root') {
      return shape('valid' => false, 'error' => "malformed 'euid' match");
    }
    $invert = false;
  } else {
    return shape('valid' => false, 'error' => "malformed 'euid' match");
  }
  $matches = runif_test_for_feature($options, $test, 'posix_geteuid() === 0');
  if ($matches !== $invert) return shape('valid' => true, 'match' => true);
  return shape(
    'valid' => true,
    'match' => false,
    'skip_reason' => 'skip-runif-euid-' . implode('-', $words)
  );
}

function runif_extension_matches(
  Options $options,
  string $test,
  vec<string> $words,
): RunifResult {
  if (count($words) !== 1) {
    return shape('valid' => false, 'error' => "malformed 'extension' match");
  }
  if (runif_test_for_feature($options, $test, "extension_loaded('{$words[0]}')")) {
    return shape('valid' => true, 'match' => true);
  }
  return shape(
    'valid' => true,
    'match' => false,
    'skip_reason' => 'skip-runif-extension-' . $words[0]
  );
}

function runif_locale_matches(
  Options $options,
  string $test,
  vec<string> $words,
): RunifResult {
  if (count($words) < 2) {
    return shape('valid' => false, 'error' => "malformed 'locale' match");
  }
  $category = array_shift(inout $words);
  if (preg_match('/^LC_[A-Z]+$/', $category) !== 1) {
    return shape('valid' => false, 'error' => "bad locale category '$category'");
  }
  $locale_args = implode(', ', array_map($word ==> "'$word'", $words));
  $matches = runif_test_for_feature(
    $options,
    $test,
    "defined('$category') && (false !== setlocale($category, $locale_args))",
  );
  if ($matches) {
    return shape('valid' => true, 'match' => true);
  }
  return shape(
    'valid' => true,
    'match' => false,
    'skip_reason' => 'skip-runif-locale',
  );
}

function runif_should_skip_test(
  Options $options,
  string $test,
): RunifResult {
  $runif_path = find_test_ext($test, 'runif');
  if ($runif_path is null) return shape('valid' => true, 'match' => true);

  $file_empty = true;
  $contents = file($runif_path, FILE_IGNORE_NEW_LINES);
  foreach ($contents as $line) {
    $line = preg_replace('/[#].*$/', '', $line); // remove comment
    $line = trim($line);
    if ($line === '') continue;
    $file_empty = false;

    $words = preg_split('/ +/', $line);
    if (count($words) < 2) {
      return shape('valid' => false, 'error' => "malformed line '$line'");
    }
    foreach ($words as $word) {
      if (preg_match('|^[\w/.-]+$|', $word) !== 1) {
        return shape(
          'valid' => false,
          'error' => "bad word '$word' in line '$line'",
        );
      }
    }

    $type = array_shift(inout $words);
    $words = vec($words); // array_shift always promotes to dict :-\
    switch ($type) {
      case 'os':
        $result = runif_os_matches($words);
        break;
      case 'file':
        $result = runif_file_matches($words);
        break;
      case 'euid':
        $result = runif_euid_matches($options, $test, $words);
        break;
      case 'extension':
        $result = runif_extension_matches($options, $test, $words);
        break;
      case 'locale':
        $result = runif_locale_matches($options, $test, $words);
        break;
      default:
        return shape('valid' => false, 'error' => "bad match type '$type'");
    }
    if (!$result['valid'] || !Shapes::idx($result, 'match', false)) {
      return $result;
    }
  }
  if ($file_empty) return shape('valid' => false, 'error' => 'empty runif file');
  return shape('valid' => true, 'match' => true);
}

<<__Memoize>>
function is_onlyfacebook_dir(string $dir): bool {
  if (file_exists($dir.'/onlyfacebook')) return true;
  if ($dir === realpath(test_dir())) return false;
  $parent = dirname($dir);
  if ($parent === $dir) return false;
  return is_onlyfacebook_dir($parent);
}

// A test is fbcode-only when it carries a "$test.onlyfacebook" marker, or when
// any directory between it and hphp/test carries an "onlyfacebook" marker.
function is_onlyfacebook_test(string $test): bool {
  if (read_test_flag($test, 'onlyfacebook')) return true;
  return is_onlyfacebook_dir(realpath(dirname($test)));
}

// should_skip_test_simple handles generating skips in ways that are purely
// based on options passed to run.php, and test names/exclusion files (eg.
// norepo).  The benefit of should_skip_test_simple is that it can factor in to
// test listing, while more complex skip behavior will appear in test results.
function should_skip_test_simple(
  Options $options,
  string $test,
): ?string {
  if (!$options->facebook_build && is_onlyfacebook_test($test)) {
    return 'skip-onlyfacebook';
  }

  if (($options->cli_server || $options->server) &&
      !can_run_server_test($test, $options)) {
    return 'skip-server';
  }

  if ($options->hhas_round_trip && substr($test, -5) === ".hhas") {
    return 'skip-hhas';
  }

  if ($options->hhbbc2 || $options->hhas_round_trip) {
    if (read_test_flag($test, 'nodumphhas') ||
        file_exists(dirname($test).'/nodumphhas')) {
      return 'skip-nodumphhas';
    }
    if (read_test_flag($test, 'verify')) {
      return 'skip-verify';
    }
  }

  if (has_multi_request_mode($options) || $options->server) {
    if (read_test_flag($test, 'verify')) {
      return 'skip-verify';
    }
    if (read_test_flag($test, 'nomultireq') ||
        file_exists(dirname($test).'/nomultireq')) {
      return 'skip-multi-req';
    }
    if (!Str\is_empty(find_debug_config($test, 'hphpd.ini'))) {
      return 'skip-debugger';
    }
  }

  if ($options->bespoke && read_test_flag($test, 'nobespoke')) {
      // Skip due to changes in array identity
      return 'skip-bespoke';
  }

  if ($options->persistent && read_test_flag($test, 'skip_persistent')) {
    return 'skip-persistent';
  }

  if ($options->jit_serialize is nonnull &&
      read_test_flag($test, 'nojitserialize')) {
    return 'skip-jit-serialize';
  }

  if ((!$options->repo || $options->jit_serialize is null
       || (int)$options->jit_serialize < 1)
      && read_test_flag($test, 'onlyjumpstart')) {
    return 'skip-onlyjumpstart';
  }

  if (!$options->repo && read_test_flag($test, 'onlyrepo')) {
    return 'skip-onlyrepo';
  }

  if ($options->repo) {
    if (read_test_flag($test, 'norepo')) return 'skip-norepo';
    if (read_test_flag($test, 'verify')) return 'skip-verify';
    if (!Str\is_empty(find_debug_config($test, 'hphpd.ini'))) return 'skip-debugger';
  }

  if ($options->only_remote_executable &&
      (read_test_flag($test, 'no_remote') ||
       read_test_flag($test, 'uses_loopback'))) {
    return 'skip-only_running_remote_executable_tests_not_requiring_loopback';
  }

  if ($options->only_remote_executable_with_loopback &&
      (read_test_flag($test, 'no_remote') ||
       !read_test_flag($test, 'uses_loopback'))) {
    return 'skip-only_running_remote_executable_tests_requiring_loopback';
  }

  if ($options->only_non_remote_executable &&
      !read_test_flag($test, 'no_remote')) {
    return 'skip-only_running_NON_remote_executable_tests';
  }

  return null;
}

function skipif_should_skip_test(
  Options $options,
  string $test,
): RunifResult {
  $skipif_test = find_test_ext($test, 'skipif');
  if ($skipif_test is null) {
    return shape('valid' => true, 'match' => true);
  }

  // Run the .skipif in non-repo mode since building a repo for it is
  // inconvenient and the same features should be available. Pick the mode
  // arbitrarily for the same reason.
  $options_without_repo = clone $options;
  $options_without_repo->repo = false;
  list($hhvm, $_) = hhvm_cmd($options_without_repo, $test, $skipif_test);
  $hhvm = $hhvm[0];
  // Remove any --count <n> from the command
  $hhvm = preg_replace('/ --count[ =]\d+/', '', $hhvm);

  $descriptorspec = dict[
    0 => vec["pipe", "r"],
    1 => vec["pipe", "w"],
    2 => vec["pipe", "w"],
  ];
  $pipes = null;
  $process = proc_open("$hhvm $test 2>&1", $descriptorspec, inout $pipes);
  if (!is_resource($process)) {
    return shape(
      'valid' => false,
      'error' => 'proc_open failed while running skipif'
    );
  }

  $pipes as nonnull;
  fclose($pipes[0]);
  $output = trim(stream_get_contents($pipes[1]));
  fclose($pipes[1]);
  proc_close($process);

  // valid output is empty or a single line starting with 'skip'
  // everything else must result in a test failure
  if ($output === '') {
    return shape('valid' => true, 'match' => true);
  }
  if (preg_match('/^skip.*$/', $output) === 1) {
    return shape(
      'valid' => true,
      'match' => false,
      'skip_reason' => 'skip-skipif',
    );
  }
  return shape('valid' => false, 'error' => "invalid skipif output '$output'");
}

class BadExpectfPattern extends Exception {
  public function __construct(string $str, string $pattern, int $offset) {
    parent::__construct(
      "$str (at offset $offset, context: \"" .
      substr($pattern, max($offset - 25, 0), 50) .
      "\")"
    );
  }
}

class ExpectfParser {
  private string $str;

  private string $pstr;
  private vec<?(function(int): vec<(int, int)>)> $pattern = vec[];
  private int $pindex = 0;

  private bool $found_wildcard = false;
  private bool $success = false;

  public function __construct(string $str, string $pattern) {
    $this->str = $str;
    $this->pstr = $pattern;
    $this->parse();
    $this->success = $this->run();
  }

  public function succeeded(): bool { return $this->success; }
  public function foundWildcard(): bool { return $this->found_wildcard; }

  private function literal(string $s): void {
    $next = count($this->pattern) + 1;
    $this->pattern[] = (int $sindex) ==> {
      if (strlen($s) === 0) return vec[tuple($sindex, $next)];
      if ($sindex + strlen($s) > strlen($this->str)) return vec[];
      if (substr_compare($this->str, $s, $sindex, strlen($s)) !== 0) {
        return vec[];
      }
      return vec[tuple($sindex + strlen($s), $next)];
    };
  }

  private function any(): void {
    $next = count($this->pattern) + 1;
    $this->pattern[] = (int $sindex)  ==> {
      if ($sindex === strlen($this->str)) return vec[];
      return vec[tuple($sindex + 1, $next)];
    };
  }

  private function anyBut(string $s): void {
    $next = count($this->pattern) + 1;
    $this->pattern[] = (int $sindex) ==> {
      if ($sindex === strlen($this->str)) return vec[];
      if ($s === $this->str[$sindex]) return vec[];
      return vec[tuple($sindex + 1, $next)];
    };
  }

  private function func((function(string): bool) $f): void {
    $next = count($this->pattern) + 1;
    $this->pattern[] = (int $sindex) ==> {
      if ($sindex === strlen($this->str)) return vec[];
      if (!$f($this->str[$sindex])) return vec[];
      return vec[tuple($sindex + 1, $next)];
    };
  }

  private function or(vec<(function(): void)> $choices): void {
    $start = count($this->pattern);
    $this->pattern[] = null;

    $choice_indices = vec[];
    $jmp_indices = vec[];
    foreach ($choices as $c) {
      $choice_indices[] = count($this->pattern);
      $c();
      $jmp_indices[] = count($this->pattern);
      $this->pattern[] = null;
    }
    $end = count($this->pattern);

    $this->pattern[$start] = (int $sindex) ==> {
      $v = vec[];
      foreach ($choice_indices as $i) $v[] = tuple($sindex, $i);
      return $v;
    };

    foreach ($jmp_indices as $i) {
      $this->pattern[$i] = (int $sindex) ==> {
        return vec[tuple($sindex, $end)];
      };
    }
  }

  private function orSub(): void {
    $start = count($this->pattern);
    $this->pattern[] = null;

    $choice_indices = vec[];
    $jmp_indices = vec[];
    $finished = false;
    do {
      $choice_indices[] = count($this->pattern);
      $finished = $this->parse('|');
      $jmp_indices[] = count($this->pattern);
      $this->pattern[] = null;
    } while (!$finished);
    $end = count($this->pattern);

    $this->pattern[$start] = (int $sindex) ==> {
      $v = vec[];
      foreach ($choice_indices as $i) $v[] = tuple($sindex, $i);
      return $v;
    };

    foreach ($jmp_indices as $i) {
      $this->pattern[$i] = (int $sindex) ==> {
        return vec[tuple($sindex, $end)];
      };
    }
  }

  private function zeroOrMore((function(): void) $c): void {
    $index1 = count($this->pattern);
    $this->pattern[] = null;
    $c();
    $this->pattern[] = (int $sindex) ==> {
      return vec[tuple($sindex, $index1)];
    };
    $index2 = count($this->pattern);
    $this->pattern[$index1] = (int $sindex) ==> {
      return vec[tuple($sindex, $index1+1), tuple($sindex, $index2)];
    };
  }

  private function oneOrMore((function(): void) $c): void {
    $start = count($this->pattern);
    $c();
    $end = count($this->pattern);
    $this->pattern[] = (int $sindex) ==> {
      return vec[tuple($sindex, $start), tuple($sindex, $end + 1)];
    };
  }

  private function opt((function(): void) $c): void {
    $index1 = count($this->pattern);
    $this->pattern[] = null;
    $c();
    $index2 = count($this->pattern);
    $this->pattern[$index1] = (int $sindex) ==> {
      return vec[tuple($sindex, $index1+1), tuple($sindex, $index2)];
    };
  }

  private function plusMinus(): void {
    $this->or(vec[() ==> $this->literal('-'), () ==> $this->literal('+')]);
  }

  private function whitespace(): void {
    $this->func(ctype_space<>);
  }
  private function digit(): void {
    $this->func(ctype_digit<>);
  }
  private function hexdigit(): void {
    $this->func(ctype_xdigit<>);
  }
  private function notnewline(): void {
    $this->anyBut("\n");
  }

  private function parse(?string $sub = null): bool {
    $current_literal = '';

    $size = strlen($this->pstr);
    while ($this->pindex < $size) {
      $token = $this->pstr[$this->pindex];
      ++$this->pindex;
      if ($token !== '%') {
        if ($sub is nonnull) {
          if ($token === '}') {
            if (strlen($current_literal) !== 0) $this->literal($current_literal);
            return true;
          }
          if ($token === '|' && $sub === '|') {
            if (strlen($current_literal) !== 0) $this->literal($current_literal);
            return false;
          }
        }
        $current_literal .= $token;
        continue;
      }

      if ($this->pindex == $size) {
        throw new BadExpectfPattern(
          "Unterminated wildcard at end of pattern", $this->pstr, $this->pindex
        );
      }
      $token = $this->pstr[$this->pindex];
      ++$this->pindex;

      if ($token !== '%' && $token !== 't' && $token !== 'h') {
        $this->found_wildcard = true;
        if (strlen($current_literal) !== 0) {
          $this->literal($current_literal);
          $current_literal = '';
        }
      }

      switch ($token) {
        case '%':
          $current_literal .= '%';
          break;
        case 't':
          $current_literal .= "\t";
          break;
        case 'c':
          $this->any();
          break;
        case 'd':
          $this->oneOrMore(() ==> $this->digit());
          break;
        case 'x':
          $this->oneOrMore(() ==> $this->hexdigit());
          break;
        case 's':
          $this->oneOrMore(() ==> $this->notnewline());
          break;
        case 'a':
          $this->oneOrMore(() ==> $this->any());
          break;
        case 'w':
          $this->zeroOrMore(() ==> $this->whitespace());
          break;
        case 'C':
          $this->opt(() ==> $this->any());
          break;
        case 'S':
          $this->zeroOrMore(() ==> $this->notnewline());
          break;
        case 'A':
          $this->zeroOrMore(() ==> $this->any());
          break;
        case 'i':
          $this->opt(() ==> $this->plusMinus());
          $this->oneOrMore(() ==> $this->digit());
          break;
        case 'f':
          // This is more permissive than necessary, but good enough.
          $this->opt(() ==> $this->plusMinus());
          $this->opt(() ==> $this->literal('.'));
          $this->oneOrMore(() ==> $this->digit());
          $this->opt(() ==> $this->literal('.'));
          $this->zeroOrMore(() ==> $this->digit());
          $this->opt(() ==> {
            $this->or(
              vec[() ==> $this->literal('E'), () ==> $this->literal('e')]
            );
            $this->opt(() ==> $this->plusMinus());
            $this->oneOrMore(() ==> $this->digit());
          });
          break;
        case 'h':
          if ($this->pindex == $size) {
            throw new BadExpectfPattern(
              "While parsing %h, expected '{', but reached end of pattern",
              $this->pstr, $this->pindex
            );
          }
          $brace = $this->pstr[$this->pindex];
          if ($brace !== '{') {
            throw new BadExpectfPattern(
              "While parsing %h, expected '{', but got '$brace'",
              $this->pstr, $this->pindex
            );
          }
          ++$this->pindex;
          if ($this->pindex == $size) {
            throw new BadExpectfPattern(
              "While parsing %h, expected hex digit, but reached end of pattern",
              $this->pstr, $this->pindex
            );
          }
          $h1 = $this->pstr[$this->pindex];
          if (!ctype_xdigit($h1)) {
            throw new BadExpectfPattern(
              "While parsing %h, expected hex digit, but got '$h1'",
              $this->pstr, $this->pindex
            );
          }
          ++$this->pindex;
          if ($this->pindex == $size) {
            throw new BadExpectfPattern(
              "While parsing %h, expected hex digit, but reached end of pattern",
              $this->pstr, $this->pindex
            );
          }
          $h2 = $this->pstr[$this->pindex];
          if (!ctype_xdigit($h2)) {
            throw new BadExpectfPattern(
              "While parsing %h, expected hex digit, but got '$h2'",
              $this->pstr, $this->pindex
            );
          }
          ++$this->pindex;
          if ($this->pindex == $size) {
            throw new BadExpectfPattern(
              "While parsing %h, expected '}', but reached end of pattern",
              $this->pstr, $this->pindex
            );
          }
          $brace = $this->pstr[$this->pindex];
          if ($brace !== '}') {
            throw new BadExpectfPattern(
              "While parsing %h, expected '}', but got '$brace'",
              $this->pstr, $this->pindex
            );
          }
          ++$this->pindex;
          $current_literal .= chr(hexdec($h1 . $h2));
          break;
        case '|':
          if ($this->pindex == $size) {
            throw new BadExpectfPattern(
              "While parsing %|, expected '{', but reached end of pattern",
              $this->pstr, $this->pindex
            );
          }
          $brace = $this->pstr[$this->pindex];
          if ($brace !== '{') {
            throw new BadExpectfPattern(
              "While parsing %|, expected '{', but got '$brace'",
              $this->pstr, $this->pindex
            );
          }
          ++$this->pindex;
          $this->orSub();
          break;
        case '?':
          if ($this->pindex == $size) {
            throw new BadExpectfPattern(
              "While parsing %?, expected '{', but reached end of pattern",
              $this->pstr, $this->pindex
            );
          }
          $brace = $this->pstr[$this->pindex];
          if ($brace !== '{') {
            throw new BadExpectfPattern(
              "While parsing %?, expected '{', but got '$brace'",
              $this->pstr, $this->pindex
            );
          }
          ++$this->pindex;
          $this->opt(() ==> { $this->parse('?'); return; });
          break;
        case '*':
          if ($this->pindex == $size) {
            throw new BadExpectfPattern(
              "While parsing %*, expected '{', but reached end of pattern",
              $this->pstr, $this->pindex
            );
          }
          $brace = $this->pstr[$this->pindex];
          if ($brace !== '{') {
            throw new BadExpectfPattern(
              "While parsing %*, expected '{', but got '$brace'",
              $this->pstr, $this->pindex
            );
          }
          ++$this->pindex;
          $this->zeroOrMore(() ==> { $this->parse('*'); return; });
          break;
        default:
          throw new BadExpectfPattern(
            "Unknown wildcard %$token (escape with % if not a wildcard)",
            $this->pstr, $this->pindex
          );
      }
    }

    if ($sub is nonnull) {
      throw new BadExpectfPattern(
        "While parsing %$sub, expected '}', but reached end of pattern",
        $this->pstr, $this->pindex
      );
    }
    if (strlen($current_literal) !== 0) $this->literal($current_literal);
    return false;
  }

  private function run(): bool {
    $states = dict[];
    $states[0] = keyset[];
    $states[0][] = 0;

    do {
      $newStates = dict[];
      foreach ($states as $pindex => $sindices) {
        foreach ($sindices as $sindex) {
          if ($pindex === count($this->pattern)) {
            if ($sindex === strlen($this->str)) return true;
            continue;
          }
          foreach (($this->pattern[$pindex] as nonnull)($sindex) as list($s, $p)) {
            $newStates[$p] ??= keyset[];
            $newStates[$p][] = $s;
          }
        }
      }
      $states = $newStates;
    } while (count($states) !== 0);
    return false;
  }
}

function count_array_diff(
  vec<string> $ar1, vec<string> $ar2,
  int $idx1, int $idx2, int $cnt1, int $cnt2, num $steps,
  (function(string, string): bool) $cmp
): int {
  $equal = 0;

  while ($idx1 < $cnt1 && $idx2 < $cnt2 && $cmp($ar1[$idx1], $ar2[$idx2])) {
    $idx1++;
    $idx2++;
    $equal++;
    $steps--;
  }
  $steps--;
  if ($steps > 0) {
    $eq1 = 0;
    $st = $steps / 2;

    for ($ofs1 = $idx1 + 1; $ofs1 < $cnt1 && $st > 0; $ofs1++) {
      $st--;
      $eq = count_array_diff($ar1, $ar2, $ofs1, $idx2, $cnt1,
                             $cnt2, $st, $cmp);

      if ($eq > $eq1) {
        $eq1 = $eq;
      }
    }

    $eq2 = 0;
    $st = $steps;

    for ($ofs2 = $idx2 + 1; $ofs2 < $cnt2 && $st > 0; $ofs2++) {
      $st--;
      $eq = count_array_diff($ar1, $ar2, $idx1, $ofs2, $cnt1,
                             $cnt2, $st, $cmp);
      if ($eq > $eq2) {
        $eq2 = $eq;
      }
    }

    if ($eq1 > $eq2) {
      $equal += $eq1;
    } else if ($eq2 > 0) {
      $equal += $eq2;
    }
  }

  return $equal;
}

function generate_array_diff(
  vec<string> $ar1,
  vec<string> $ar2,
  vec<string> $w,
  (function(string, string): bool) $cmp
): vec<string> {
  $idx1 = 0; $cnt1 = count($ar1);
  $idx2 = 0; $cnt2 = count($ar2);
  $old1 = dict[];
  $old2 = dict[];

  while ($idx1 < $cnt1 && $idx2 < $cnt2) {
    if ($cmp($ar1[$idx1], $ar2[$idx2])) {
      $idx1++;
      $idx2++;
      continue;
    } else {
      $c1 = count_array_diff($ar1, $ar2, $idx1+1, $idx2, $cnt1,
                             $cnt2, 10, $cmp);
      $c2 = count_array_diff($ar1, $ar2, $idx1, $idx2+1, $cnt1,
                             $cnt2, 10, $cmp);

      if ($c1 > $c2) {
        $old1[$idx1+1] = sprintf("%03d- ", $idx1+1) . $w[$idx1];
        $idx1++;
      } else if ($c2 > 0) {
        $old2[$idx2+1] = sprintf("%03d+ ", $idx2+1) . $ar2[$idx2];
        $idx2++;
      } else {
        $old1[$idx1+1] = sprintf("%03d- ", $idx1+1) . $w[$idx1];
        $old2[$idx2+1] = sprintf("%03d+ ", $idx2+1) . $ar2[$idx2];
        $idx1++;
        $idx2++;
      }
    }
  }

  $diff = vec[];
  $old1_keys = array_keys($old1);
  $old2_keys = array_keys($old2);
  $old1_values = array_values($old1);
  $old2_values = array_values($old2);
  // these start at -2 so $l1 + 1 and $l2 + 1 are not valid indices
  $l1 = -2;
  $l2 = -2;
  $iter1 = 0; $end1 = count($old1);
  $iter2 = 0; $end2 = count($old2);

  while ($iter1 < $end1 || $iter2 < $end2) {
    $k1 = $iter1 < $end1 ? $old1_keys[$iter1] : -2;
    $k2 = $iter2 < $end2 ? $old2_keys[$iter2] : -2;
    if ($k1 === $l1 + 1 || $iter2 >= $end2) {
      $l1 = $k1;
      $diff[] = $old1_values[$iter1];
      $iter1++;
    } else if ($k2 === $l2 + 1 || $iter1 >= $end1) {
      $l2 = $k2;
      $diff[] = $old2_values[$iter2];
      $iter2++;
    } else if ($k1 < $k2) {
      $l1 = $k1;
      $diff[] = $old1_values[$iter1];
      $iter1++;
    } else {
      $l2 = $k2;
      $diff[] = $old2_values[$iter2];
      $iter2++;
    }
  }

  while ($idx1 < $cnt1) {
    $diff[] = sprintf("%03d- ", $idx1 + 1) . $w[$idx1];
    $idx1++;
  }

  while ($idx2 < $cnt2) {
    $diff[] = sprintf("%03d+ ", $idx2 + 1) . $ar2[$idx2];
    $idx2++;
  }

  return $diff;
}

function escape_unprintables(string $str): string {
  $out = '';
  for ($i = 0; $i < strlen($str); $i++) {
    $s = $str[$i];
    if (ctype_print($s)) {
      $out .= $s;
    } else if ($s === "\n") {
      $out .= '\n';
    } else if ($s === "\r") {
      $out .= '\r';
    } else if ($s === "\t") {
      $out .= '\t';
    } else {
      $h = dechex(ord($s));
      if (strlen($h) < 2) $h = "0" . $h;
      $out .= '\x' . $h;
    }
  }
  return $out;
}

function generate_diff(
  string $wanted,
  string $output,
  (function(string, string): bool) $cmp
): string {
  $w = explode("\n", $wanted);
  $o = explode("\n", $output);
  $diff = generate_array_diff($w, $o, $w, $cmp);
  if (count($diff) > 200) {
    $diff = array_slice($diff, 0, 200);
    $diff[] = "(truncated)";
  }
  return implode("\n", array_map(escape_unprintables<>, $diff));
}

function dump_hhas_cmd(
  string $hhvm_cmd, string $test, string $hhas_file,
): string {
  $dump_flags = implode(' ', vec[
    '-vEval.AllowHhas=true',
    '-vEval.DumpHhas=1',
    '-vEval.DumpHhasToFile='.escapeshellarg($hhas_file),
    '-vEval.LoadFilepathFromUnitCache=0',
  ]);
  $cmd = str_replace(' -- ', " $dump_flags -- ", $hhvm_cmd);
  if ($cmd === $hhvm_cmd) $cmd .= " $dump_flags";
  return $cmd;
}

function dump_hhas_to_temp(string $hhvm_cmd,
                           string $test,
                           string $prefix = ''): ?string {
  $temp_file =
    Status::getTestWorkingDir($test) .  '/' . $prefix . 'round_trip.hhas';
  $cmd = dump_hhas_cmd($hhvm_cmd, $test, $temp_file);
  $ret = -1;
  system("$cmd &> /dev/null", inout $ret);
  return $ret === 0 ? $temp_file : null;
}

const vec<string> SERVER_EXCLUDE_PATHS = vec[
  'quick/xenon/',
  'slow/streams/',
  'slow/ext_mongo/',
  'slow/ext_oauth/',
  'slow/ext_vsdebug/',
  'zend/good/ext/standard/tests/array/',
];

const string HHAS_EXT = '.hhas';

function can_run_server_test(string $test, Options $options): bool {
  // explicitly disabled
  if (read_test_flag($test, 'noserver') ||
      (read_test_flag($test, 'nowebserver') && $options->server)) {
    return false;
  }

  $config = read_test_config($test);
  if ($options->server &&
      (C\contains_key($config, 'env') ||
       C\contains_key($config, 'clisrv_env'))) {
    return false;
  }

  // has its own config
  if (C\contains_key(read_test_config($test), 'opts') ||
      is_file("$test.ini") ||
      is_file("$test.use.for.ini.migration.testing.only.hdf")) {
    return false;
  }

  // we can't run repo only tests in server modes
  if (read_test_flag($test, 'onlyrepo') ||
      read_test_flag($test, 'onlyjumpstart')) {
    return false;
  }

  foreach (SERVER_EXCLUDE_PATHS as $path) {
    if (strpos($test, $path) !== false) return false;
  }

  // don't run hhas tests in server modes
  if (strrpos($test, HHAS_EXT) === (strlen($test) - strlen(HHAS_EXT))) {
    return false;
  }

  return true;
}

const int SERVER_TIMEOUT = 60;

function run_config_server(Options $options, string $test): mixed {
  invariant(
    can_run_server_test($test, $options),
    'should_skip_test_simple should have skipped this',
  );

  $config = find_file_for_dir(dirname($test), 'config.ini') ?? '';
  $servers = $options->servers as Servers;
  $port = $servers->configs[$config]->port;
  $ch = curl_init("localhost:$port/$test");
  curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
  curl_setopt($ch, CURLOPT_TIMEOUT, SERVER_TIMEOUT);
  curl_setopt($ch, CURLOPT_BINARYTRANSFER, true);
  $output = curl_exec($ch);
  if ($output is string) {
    $output = trim($output);
  } else {
    $output = "Error talking to server: " . curl_error($ch);
  }
  curl_close($ch);

  return run_config_post(null, $output, $test, $options, false);
}

function run_config_cli(
  string $test,
  string $cmd,
  dict<string, mixed> $cmd_env,
  bool $assert_verify = false
): ?string {
  $exit_codes = $assert_verify ? keyset[255, 127] : keyset[0, 1];
  $expected = read_test_value($test, 'exit_code');
  if ($expected is nonnull) {
    $exit_codes = keyset[(int)trim($expected)];
  }
  list($output, $success) = exec_with_timeout($cmd, $cmd_env, $exit_codes);
  if (!$success) {
    Status::writeDiff($test, $output);
    return null;
  }
  return $output;
}

function replace_object_resource_ids(string $str, string $replacement): string {
  $str = preg_replace(
    '/(object\([^)]+\)#)\d+/', '\1'.$replacement, $str
  );
  return preg_replace(
    '/resource\(\d+\)/', "resource($replacement)", $str
  );
}

function run_config_post(
  ?string $pre_output,
  string $output,
  string $test,
  Options $options,
  bool $assert_verify
): bool {
  list($file, $type) = get_expect_file_and_type($test, $options);
  if ($file is null || $type is null) {
    Status::writeDiff(
      $test,
      "No $test.expect or $test.expectf. " .
      "If $test is meant to be included by other tests, " .
      "use a different file extension.\n"
    );
    return false;
  }

  $repeats = 0;
  if (!$assert_verify) {
    if ($options->retranslate_all is nonnull) {
      $repeats = (int)$options->retranslate_all * 2;
    }
    if ($options->free_random_units is nonnull) {
      $repeats = (int)$options->free_random_units;
    }
    if ($options->repeat is nonnull) {
      $repeats = (int)$options->repeat;
    }
    if ($options->recycle_tc is nonnull) {
      $repeats = (int)$options->recycle_tc;
    }
    if ($options->cli_server) {
      $repeats = 3;
    }
  }

  $prep = ($output) ==> {
    $output = trim($output);
    if ($pre_output is nonnull) {
      $output = $pre_output . $output;
    }
    return $output;
  };
  if ($repeats === 0) {
    $split = vec[$prep($output)];
  } else {
    $split = array_map($prep, explode(MULTI_REQUEST_SEP, $output));
  }

  $output = str_replace(MULTI_REQUEST_SEP, '', $output);
  if ($pre_output is nonnull) {
    $output = $pre_output . $output;
  }
  file_put_contents(Status::getTestWorkingDir($test) . '/out', $output);

  if ($repeats > 0 && count($split) != $repeats) {
    Status::writeDiff(
      $test,
      count($split) . ' sets of output returned, expected ' . $repeats
    );
    return false;
  }

  $wanted = trim(file_get_contents($file));

  if ($type === 'expect') {
    if ($options->ignore_oids is nonnull || $options->repo) {
      $split = array_map($s ==> replace_object_resource_ids($s, 'n'), $split);
      $wanted = replace_object_resource_ids($wanted, 'n');
    }

    foreach ($split as $s) {
      if (strcmp($s, $wanted)) {
        Status::writeDiff(
          $test,
          generate_diff($wanted, $s, ($l1, $l2) ==> !strcmp($l1, $l2))
        );
        return false;
      }
    }
    return true;
  } else if ($type === 'expectf') {
    if ($options->ignore_oids is nonnull || $options->repo) {
      $wanted = replace_object_resource_ids($wanted, '%d');
    }

    try {
      foreach ($split as $s) {
        $parser = new ExpectfParser($s, $wanted);
        if (!$parser->foundWildcard()) {
          Status::writeDiff(
            $test,
            'Bad expectf file: File contains no actual wildcards. ' .
            'Use expect instead'
          );
          return false;
        }
        if (!$parser->succeeded()) {
          $diff = generate_diff(
            $wanted,
            $s,
            ($l1, $l2) ==> (new ExpectfParser($l2, $l1))->succeeded()
          );
          Status::writeDiff($test, $diff);
          return false;
        }
      }
      return true;
    } catch (BadExpectfPattern $e) {
      Status::writeDiff($test, 'Bad expectf file: ' . $e->getMessage());
      return false;
    }
  } else {
    throw new Exception("Unsupported expect file type: ".$type);
  }
}

function run_foreach_config(
  Options $options,
  string $test,
  vec<string> $cmds,
  dict<string, mixed> $cmd_env,
  bool $assert_verify = false,
  ?string $pre_output = null
): bool {
  invariant(count($cmds) > 0, "run_foreach_config: no modes");
  $result = false;
  foreach ($cmds as $cmd) {
    $output = run_config_cli($test, $cmd, $cmd_env, $assert_verify);
    if ($output is null) return false;
    $result = run_config_post($pre_output, $output, $test, $options, $assert_verify);
    if (!$result) return $result;
  }
  return $result;
}

function run_and_log_test(Options $options, string $test): void {
  $stime = time();
  $time = mtime();
  Status::createTestWorkingDir($test);
  $status = run_test($options, $test);
  $time = mtime() - $time;
  $etime = time();

  if ($status === false) {
    $diff = Status::diffForTest($test);
    if ($diff === '') {
      $diff = 'Test failed with empty diff';
    }
    Status::fail($test, $time, $stime, $etime, $diff);
  } else if ($status === true) {
    Status::pass($test, $time, $stime, $etime);
    clean_test_files($test, $options);
  } else if ($status is string) {
    invariant(
      preg_match('/^skip-[\w-]+$/', $status) === 1,
      "invalid skip status %s",
      $status
    );
    Status::skip($test, substr($status, 5), $time, $stime, $etime);
    clean_test_files($test, $options);
  } else {
    invariant_violation("invalid status type %s", gettype($status));
  }
}

// Returns "(string | bool)".
function run_test(Options $options, string $test): mixed {
  $skip_reason = should_skip_test_simple($options, $test);
  if ($skip_reason is nonnull) return $skip_reason;

  if (!$options->no_skipif) {
    $result = runif_should_skip_test($options, $test);
    if (!$result['valid']) {
      invariant(Shapes::keyExists($result, 'error'), 'missing runif error');
      Status::writeDiff($test, 'Invalid .runif file: ' . $result['error']);
      return false;
    }
    if (!($result['match'] ?? false)) {
      invariant(Shapes::keyExists($result, 'skip_reason'), 'missing skip_reason');
      return $result['skip_reason'];
    }

    $result = skipif_should_skip_test($options, $test);
    if (!$result['valid']) {
      invariant(Shapes::keyExists($result, 'error'), 'missing skipif error');
      Status::writeDiff($test, $result['error']);
      return false;
    }
    if (!($result['match'] ?? false)) {
      invariant(Shapes::keyExists($result, 'skip_reason'), 'missing skip_reason');
      return $result['skip_reason'];
    }
  }

  if (sbcc_uses_builder($test) && sbcc_build_binary() is null) {
    return 'skip-sbcc-build';
  }

  // SBCC pre-test setup: build or copy the .sbcc artifact before HHVM starts.
  if (sbcc_enabled($test)) {
    list($sbcc_output, $sbcc_success) = sbcc_mode_setup($options, $test);
    if (!$sbcc_success) {
      Status::writeDiff($test, $sbcc_output);
      return false;
    }
  }

  list($hhvm, $hhvm_env) = hhvm_cmd($options, $test);

  if (HH\legacy_is_truthy(preg_grep('/ --count[ =][0-9]+ .* --count[ =][0-9]+( |$)/', $hhvm))) {
    // we got --count from 2 sources (e.g. .opts file and multi_request_mode)
    // this can't work so skip the test
    return 'skip-count';
  } else if ($options->jit_serialize is nonnull) {
    // jit-serialize adds the --count option later, so even 1 --count in the
    // command means we have to skip
    if (HH\legacy_is_truthy(preg_grep('/ --count[ =][0-9]+( |$)/', $hhvm))) {
      return 'skip-count';
    }
  }

  if ($options->repo) {
    return run_repo_test($options, $test, $hhvm, $hhvm_env);
  }

  if ($options->hhas_round_trip) {
    invariant(
      substr($test, -5) !== ".hhas",
      'should_skip_test_simple should have skipped this',
    );
    // dumping hhas, not running code so arbitrarily picking a mode
    $hhas_temp = dump_hhas_to_temp($hhvm[0], $test);
    if ($hhas_temp is null) {
      $err = "system failed: " .
        dump_hhas_cmd(
          $hhvm[0],
          $test,
          Status::getTestWorkingDir($test) . '/round_trip.hhas'
      ) . "\n";
      Status::writeDiff($test, $err);
      return false;
    }
    list($hhvm, $hhvm_env) = hhvm_cmd($options, $test, $hhas_temp);
  }

  if ($options->server) {
    return run_config_server($options, $test);
  }
  return run_foreach_config($options, $test, $hhvm, $hhvm_env);
}

// Returns "(string | bool)".
function run_repo_test(
  Options $options,
  string $test,
  vec<string> $hhvm,
  dict<string, mixed> $hhvm_env,
): mixed {
  if (file_exists($test . '.hphpc_assert')) {
    $hphp = hphp_cmd($options, $test);
    return run_foreach_config($options, $test, vec[$hphp], $hhvm_env, true);
  } else if (file_exists($test . '.hhbbc_assert')) {
    $hphp = hphp_cmd($options, $test);
    if (repo_separate($options, $test)) {
      list($output, $success) = exec_with_timeout($hphp);
      if (!$success) {
        Status::writeDiff($test, $output);
        return false;
      }
      $hhbbc = hhbbc_cmd($options, $test);
      return run_foreach_config($options, $test, vec[$hhbbc], $hhvm_env, true);
    } else {
      return run_foreach_config($options, $test, vec[$hphp], $hhvm_env, true);
    }
  }

  list($repo_output, $repo_success) = repo_mode_compile($options, $test);
  if (!$repo_success) {
    Status::writeDiff($test, $repo_output);
    return false;
  }

  if ($options->hhbbc2) {
    invariant(
      count($hhvm) === 1,
      "get_options forbids modes because we're not running code"
    );
    $hhas_temp1 = dump_hhas_to_temp($hhvm[0], $test, 'before');
    if ($hhas_temp1 is null) {
      Status::writeDiff($test, "dumping hhas after first hhbbc pass failed");
      return false;
    }
    $working_dir = Status::getTestWorkingDir($test);
    shell_exec("mv $working_dir/hhvm.hhbbc $working_dir/hhvm.hhbc");
    $hhbbc = hhbbc_cmd($options, $test);
    list($output, $success) = exec_with_timeout($hhbbc);
    if (!$success) {
      Status::writeDiff($test, $output);
      return false;
    }
    $hhas_temp2 = dump_hhas_to_temp($hhvm[0], $test, 'after');
    if ($hhas_temp2 is null) {
      Status::writeDiff($test, "dumping hhas after second hhbbc pass failed");
      return false;
    }
    $diff = shell_exec("diff $hhas_temp1 $hhas_temp2");
    if (trim($diff) !== '') {
      Status::writeDiff($test, $diff);
      return false;
    }
  }

  if ($options->jit_serialize is nonnull) {
    invariant(count($hhvm) === 1, 'get_options enforces jit mode only');
    $cmd = jit_serialize_option($hhvm[0], $test, $options, true);
    $output = run_config_cli($test, $cmd, $hhvm_env);
    if ($output is null) return false;
    $cmd = jit_serialize_option($hhvm[0], $test, $options, true);
    $output = run_config_cli($test, $cmd, $hhvm_env);
    if ($output is null) return false;
    $hhvm[0] = jit_serialize_option($hhvm[0], $test, $options, false);
  }

  return run_foreach_config($options, $test, $hhvm, $hhvm_env, false, $repo_output);
}

function num_cpus(): int {
  /* HH_FIXME[4489]: Revealed through strict switches */
  switch (PHP_OS) {
    case 'Linux':
      $data = file('/proc/stat');
      $cores = 0;
      foreach($data as $line) {
        if (preg_match('/^cpu[0-9]/', $line) === 1) {
          $cores++;
        }
      }
      return $cores;
    case 'Darwin':
    case 'FreeBSD':
      $output = null;
      $return_var = -1;
      return (int)exec('sysctl -n hw.ncpu', inout $output, inout $return_var);
  }
  return 2; // default when we don't know how to detect.
}

function make_header(string $str): string {
  return "\n\033[0;33m".$str."\033[0m\n";
}

function print_commands(
  vec<string> $tests,
  Options $options,
): void {
  if (C\count($tests) === 0) {
    print make_header(
      "Test run failed with no failed tests; did a worker process die?"
    );
  } else if ($options->verbose) {
    print make_header("Run these by hand:");
  } else {
    $test = $tests[0];
    print make_header("Run $test by hand:");
    $tests = vec[$test];
  }

  foreach ($tests as $test) {
    list($commands, $env) = hhvm_cmd($options, $test);

    $envstr = '';
    foreach (get_test_env($options, $test, dict[]) as $k => $v) {
      if (strlen($envstr) > 0) $envstr .= ' ';
      $envstr .= $k . "=" . (string)$v;
    }
    foreach ($env as $k => $v) {
      if (strpos($k, "HPHP_TEST_") === 0 || $k === 'TMPDIR') {
        if (strlen($envstr) > 0) $envstr .= ' ';
        $envstr .= $k . "=" . (string)$v;
      }
    }
    if (strlen($envstr) > 0) $envstr .= ' ';

    if (!$options->repo) {
      foreach ($commands as $c) {
        print $envstr . $c . "\n";
      }
      continue;
    }

    // How to run it with hhbbc:
    $hhbbc_cmds = hphp_cmd($options, $test)."\n";
    if (repo_separate($options, $test)) {
      $hhbbc_cmd  = hhbbc_cmd($options, $test)."\n";
      $hhbbc_cmds .= $hhbbc_cmd;
      if ($options->hhbbc2) {
        $working_dir = Status::getTestWorkingDir($test);
        foreach ($commands as $c) {
          $hhbbc_cmds .=
            $c." -vEval.DumpHhas=1 > $working_dir.before.round_trip.hhas\n";
        }
        $hhbbc_cmds .=
          "mv $working_dir/hhvm.hhbbc $working_dir/hhvm.hhbc\n";
        $hhbbc_cmds .= $hhbbc_cmd;
        foreach ($commands as $c) {
          $hhbbc_cmds .=
            $c." -vEval.DumpHhas=1 > $working_dir.after.round_trip.hhas\n";
        }
        $hhbbc_cmds .=
          "diff $working_dir.before.round_trip.hhas $working_dir.after.round_trip.hhas\n";
      }
    }
    if ($options->jit_serialize is nonnull) {
      invariant(count($commands) === 1, 'get_options enforces jit mode only');
      $hhbbc_cmds .=
        $envstr . jit_serialize_option($commands[0], $test, $options, true) . "\n";
      $hhbbc_cmds .=
        $envstr . jit_serialize_option($commands[0], $test, $options, true) . "\n";
      $commands[0] = jit_serialize_option($commands[0], $test, $options, false);
    }
    foreach ($commands as $c) {
      $hhbbc_cmds .= $envstr . $c . "\n";
    }
    print "$hhbbc_cmds\n";
  }
}

// This runs only in the "printer" child.
function msg_loop(int $num_tests, Queue $queue): void {
  $cols = null;
  $do_progress =
    $num_tests > 0 &&
    (
      Status::getMode() === Status::MODE_NORMAL ||
      Status::getMode() === Status::MODE_RECORD_FAILURES
    ) &&
    Status::hasCursorControl();
  if ($do_progress) {
    $stty = strtolower(Status::getSTTY());
    $matches = vec[];
    if (preg_match_with_matches("/columns ([0-9]+);/", $stty, inout $matches) === 1||
        // because BSD has to be different
        preg_match_with_matches("/([0-9]+) columns;/", $stty, inout $matches) === 1) {
      $cols = (int)$matches[1];
    }
  }

  while (true) {
    list($pid, $type, $message) = $queue->receiveMessage();
    if (!Status::handle_message($type, $message)) break;

    if ($cols is nonnull) {
      $total_run = (Status::$skipped + Status::$failed + Status::$passed);
      $bar_cols = $cols - 45;

      $passed_ticks  = (int)round($bar_cols * (Status::$passed / $num_tests));
      $skipped_ticks = (int)round($bar_cols * (Status::$skipped / $num_tests));
      $failed_ticks  = (int)round($bar_cols * (Status::$failed / $num_tests));

      $fill = $bar_cols - ($passed_ticks + $skipped_ticks + $failed_ticks);
      if ($fill < 0) $fill = 0;

      $passed_ticks = str_repeat('#', $passed_ticks);
      $skipped_ticks = str_repeat('#', $skipped_ticks);
      $failed_ticks = str_repeat('#', $failed_ticks);
      $fill = str_repeat('-', (int)$fill);

      echo
        "\033[2K\033[1G[",
        "\033[0;32m$passed_ticks",
        "\033[33m$skipped_ticks",
        "\033[31m$failed_ticks",
        "\033[0m$fill] ($total_run/$num_tests) ",
        "(", Status::$skipped, " skipped,", Status::$failed, " failed)";
    }
  }

  if ($cols is nonnull) {
    print "\033[2K\033[1G";
    if (Status::$skipped > 0) {
      print Status::$skipped ." tests \033[1;33mskipped\033[0m\n";
      $reasons = Status::$skip_reasons;
      arsort(inout $reasons);
      Status::$skip_reasons = $reasons as dict<_, _>;
      foreach (Status::$skip_reasons as $reason => $count) {
        printf("%12s: %d\n", $reason, $count);
      }
    }
  }
}

function print_success(
  vec<string> $tests,
  dict<string, TestResult> $results,
  Options $options,
): void {
  // We didn't run any tests, not even skipped. Clowntown!
  if (count($tests) === 0) {
    print "\nCLOWNTOWN: No tests!\n";
    if (!$options->no_fun) {
      print_clown();
    }
    return;
  }
  $ran_tests = false;
  foreach ($results as $result) {
    // The result here will either be skipped or passed (since failed is
    // handled in print_failure.
    if ($result['status'] === 'passed') {
      $ran_tests = true;
      break;
    }
  }
  // We just had skipped tests
  if (!$ran_tests) {
    print "\nSKIP-ALOO: Only skipped tests!\n";
    if (!$options->no_fun) {
      print_skipper();
    }
    return;
  }
  print "\nAll tests passed.\n";
  if (!$options->no_fun) {
    print_ship();
  }
  if ($options->failure_file is nonnull) {
    unlink($options->failure_file);
  }
  if ($options->verbose) {
    print_commands($tests, $options);
  }
}

function print_failure(
  vec<string> $argv,
  dict<string, TestResult> $results,
  Options $options,
): void {
  $failed = vec[];
  $passed = vec[];
  foreach ($results as $result) {
    if ($result['status'] === 'failed') {
      $failed[] = $result['name'];
    } else if ($result['status'] === 'passed') {
      $passed[] = $result['name'];
    }
  }
  sort(inout $failed);

  $failing_tests_file = $options->failure_file ??
    Status::getWorkingDir() . '/test-failures';
  file_put_contents($failing_tests_file, implode("\n", $failed)."\n");
  if (count($passed) !== 0) {
    $passing_tests_file = Status::getWorkingDir() . '/tests-passed';
    file_put_contents($passing_tests_file, implode("\n", $passed)."\n");
  } else {
    $passing_tests_file = "";
  }

  print "\n".count($failed)." tests failed\n";
  if (!$options->no_fun) {
    // Unicode for table-flipping emoticon
    // https://knowyourmeme.com/memes/flipping-tables
    print "(\u{256F}\u{00B0}\u{25A1}\u{00B0}\u{FF09}\u{256F}\u{FE35} \u{253B}";
    print "\u{2501}\u{253B}\n";
  }

  print_commands($failed, $options);

  print make_header("See failed test output and expectations:");
  foreach ($failed as $n => $test) {
    if ($n !== 0) print "\n";
    print 'cat ' . Status::getTestWorkingDir($test) . "/diff\n";
    print 'cat ' . Status::getTestWorkingDir($test) . "/out\n";
    $expect_file = get_expect_file_and_type($test, $options)[0];
    if ($expect_file is null) {
      print "# no expect file found for $test\n";
    } else {
      print "cat $expect_file\n";
    }

    // only print 3 tests worth unless verbose is on
    if ($n === 2 && !$options->verbose) {
      $remaining = count($failed) - 1 - $n;
      if ($remaining > 0) {
        print make_header("... and $remaining more.");
      }
      break;
    }
  }

  if (count($passed) !== 0) {
    print make_header(
      'For xargs, lists of failed and passed tests are available using:'
    );
    print 'cat '.$failing_tests_file."\n";
    print 'cat '.$passing_tests_file."\n";
  } else {
    print make_header('For xargs, list of failures is available using:').
      'cat '.$failing_tests_file."\n";
  }

  print
    make_header("Re-run just the failing tests:") .
    str_replace("run.php", "run", $argv[0]) . ' ' .
    implode(' ', \HH\global_get('recorded_options')) .
    sprintf(' $(cat %s)%s', $failing_tests_file, "\n");
}

function port_is_listening(int $port): bool {
  $socket = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
  try {
    return socket_connect($socket, 'localhost', $port);
  }
  catch (ErrorException $_) {
    return false;
  }
}

function find_open_port(): int {
  for ($i = 0; $i < 50; ++$i) {
    $port = rand(1024, 65535);
    if (!port_is_listening($port)) return $port;
  }

  error("Couldn't find an open port");
}

function start_server_proc(
  Options $options,
  string $config,
  int $port,
): Server {
  $working_dir = Status::createServerWorkingDir();
  if ($options->cli_server) {
    $cli_sock = "$working_dir/hhvm-cli";
  } else {
    // still want to test that an unwritable socket works...
    $cli_sock = '/var/run/hhvm-cli.sock';
  }
  $threads = get_num_threads($options);
  $thread_option = $options->cli_server
    ? '-vEval.UnixServerWorkers='.$threads
    : '-vServer.ThreadCount='.$threads;
  $prelude = $options->server
    ? '-vEval.PreludePath=' . Status::getWorkingDir() . '/server-prelude.php'
    : "";
  $command = hhvm_cmd_impl(
    $options,
    basename($working_dir),
    $config,
    // Provide a temporary file for `Autoload.DB.Path`: without this, we'll fail
    // to run the server with native autoloading.
    "$working_dir/autoloadDB",
    '-m', 'server',
    "-vServer.Port=$port",
    "-vServer.Type=proxygen",
    "-vAdminServer.Port=0",
    $thread_option,
    '-vServer.ExitOnBindFail=1',
    '-vServer.RequestTimeoutSeconds='.SERVER_TIMEOUT,
    '-vPageletServer.ThreadCount=0',
    '-vLog.UseRequestLog=1',
    '-vLog.File=/dev/null',
    $prelude,

    // The server will unlink the temp file
    '-vEval.UnixServerPath='.$cli_sock,

    // This ensures we actually jit everything:
    '-vEval.JitRequireWriteLease=1',

    // The default test config uses a small TC but we'll be running thousands
    // of tests against the same process:
    '-vEval.JitASize=394264576',
    '-vEval.JitGlobalDataSize=32000000',

    // load/store counters don't work on Ivy Bridge so disable for tests
    '-vEval.ProfileHWEnable=false'
  );
  if (count($command) !== 1) {
    error("Can't run multi-mode tests in server mode");
  }
  $command = $command[0];
  if (getenv('HHVM_TEST_SERVER_LOG')) {
    echo "Starting server '$command'\n";
  }

  $descriptors = dict[
    0 => vec['file', '/dev/null', 'r'],
    1 => vec['file', '/dev/null', 'w'],
    2 => vec['file', '/dev/null', 'w'],
  ];

  $dummy = null;
  $proc = proc_open($command, $descriptors, inout $dummy);
  if (!$proc) {
    error("Failed to start server process");
  }
  $status = proc_get_status($proc); // dict<string, mixed>
  $pid = $status['pid'] as int;
  $server = new Server($proc, $pid, $port, $config, $cli_sock);
  return $server;
}

final class Server {
  public function __construct(
    public resource $proc,
    public int $pid,
    public int $port,
    public string $config,
    public string $cli_socket,
  ) {
  }
}

final class Servers {
  public dict<int, Server> $pids = dict[];
  public dict<string, Server> $configs = dict[];
}

// For each config file in $configs, start up a server on a randomly-determined
// port.
function start_servers(
  Options $options,
  keyset<string> $configs,
): Servers {
  if ($options->server) {
    $socket_dir = Status::getSocketDir();
    $prelude = <<<'EOT'
<?hh
<<__EntryPoint>> function UNIQUE_NAME_I_DONT_EXIST_IN_ANY_TEST(): void {
  putenv("TMPDIR=BASEDIR".\HH\global_get('_SERVER')['SCRIPT_NAME']."/temp");
  putenv("HPHP_TEST_SOCKETDIR=$socket_dir");
}
EOT;
    file_put_contents(
      Status::getWorkingDir() . '/server-prelude.php',
      str_replace('BASEDIR', Status::getWorkingDir(), $prelude),
    );
  }

  $starting = vec[];
  foreach ($configs as $config) {
    $starting[] = start_server_proc($options, $config, find_open_port());
  }

  $start_time = mtime();
  $servers = new Servers();

  // Wait for all servers to come up.
  while (count($starting) > 0) {
    $still_starting = vec[];

    foreach ($starting as $server) {
      $config = $server->config;
      $pid = $server->pid;
      $port = $server->port;
      $proc = $server->proc;

      $new_status = proc_get_status($proc);

      if (!$new_status['running']) {
        if ($new_status['exitcode'] === 0) {
          error("Server exited prematurely but without error");
        }

        // We lost a race. Try another port.
        if (getenv('HHVM_TEST_SERVER_LOG')) {
          echo "\n\nLost connection race on port $port. Trying another.\n\n";
        }
        $port = find_open_port();
        $still_starting[] = start_server_proc($options, $config, $port);
      } else if (!port_is_listening($port)) {
        $still_starting[] = $server;
      } else {
        $servers->pids[$pid] = $server;
        $servers->configs[$config] = $server;
      }
    }

    $starting = $still_starting;
    $max_time = 30;
    if (mtime() - $start_time > $max_time) {
      error("Servers took more than $max_time seconds to come up");
    }

    // Take a short nap and try again.
    usleep(100000);
  }

  $elapsed = mtime() - $start_time;
  printf("Started %d servers in %.1f seconds\n\n", count($configs), $elapsed);
  return $servers;
}

function get_num_threads(Options $options): int {
  if ($options->threads is nonnull) {
    $threads = (int)$options->threads;
    if ((string)$threads !== $options->threads || $threads < 1) {
      error("--threads must be an integer >= 1");
    }
  } else {
    $threads = $options->server || $options->cli_server
      ? num_cpus() * 2 : num_cpus();
  }
  return $threads;
}

function runner_precheck(): void {
  // Basic checking for runner.
  $server = HH\global_get('_SERVER');
  $env = HH\global_get('_ENV');
  if (!HH\legacy_is_truthy($server) || !HH\legacy_is_truthy($env)) {
    error("\\HH\global_get('_SERVER')/\\HH\global_get('_ENV') variables not available");
  }
}

function error_handler(int $type,
                       string $message,
                       string $file,
                       int $line,
                       mixed $_1,
                       mixed $_2,
                       mixed $_3): bool {
  if (($type & error_reporting()) !== 0) return true;
  throw new ErrorException($message, 0, $type, $file, $line);
}

function main(vec<string> $argv): int {
  runner_precheck();

  set_error_handler(error_handler<>);
  ini_set('pcre.backtrack_limit', PHP_INT_MAX);

  list($options, $files) = get_options($argv);
  if ($options->help) {
    error(help());
  }
  $options->facebook_build = is_facebook_build($options);

  Status::createWorkingDir($options->working_dir);

  if ($options->list_tests) {
    list_tests($files, $options);
    print "\n";
    exit(0);
  }

  $tests = find_tests($files, $options);
  if ($options->shuffle) {
    shuffle(inout $tests);
  }

  // Explicit path given by --hhvm-binary-path takes priority. Then, if an
  // HHVM_BIN env var exists, and the file it points to exists, that trumps
  // any default hhvm executable path.
  if ($options->hhvm_binary_path is nonnull) {
    $binary_path = check_executable($options->hhvm_binary_path);
    putenv("HHVM_BIN=" . $binary_path);
  } else if (getenv("HHVM_BIN") !== false) {
    $binary_path = check_executable(getenv("HHVM_BIN"));
  } else {
    check_for_multiple_default_binaries();
    $binary_path = hhvm_path();
  }

  if ($options->verbose) {
    print "You are using the binary located at: " . $binary_path . "\n";
  }

  $servers = null;
  if ($options->server || $options->cli_server) {
    if ($options->server && $options->cli_server) {
      error("Server mode and CLI Server mode are mutually exclusive");
    }
    if ($options->repo) {
      error("Server mode repo tests are not supported");
    }

    /* We need to start up a separate server process for each config file
     * found. */
    $configs = keyset[];
    foreach ($tests as $test) {
      $config = find_file_for_dir(dirname($test), 'config.ini');
      if ($config is null) {
        error("Couldn't find config file for $test");
      }
      if (array_key_exists($config, $configs)) continue;
      if (should_skip_test_simple($options, $test) is nonnull) continue;
      $configs[] = $config;
    }

    $max_configs = 10;
    if (count($configs) > $max_configs) {
      error("More than $max_configs unique config files will be needed to run ".
            "the tests you specified. They may not be a good fit for server ".
            "mode. (".count($configs)." required)");
    }

    $servers = start_servers($options, $configs);
    $options->servers = $servers;
  }

  // Try to construct the buckets so the test results are ready in
  // approximately alphabetical order.
  // Get the serial tests to be in their own bucket later.
  $serial_tests = serial_only_tests($tests);

  // If we have no serial tests, we can use the maximum number of allowed
  // threads for the test running. If we have some, we save one thread for
  // the serial bucket. However if we only have one thread, we don't split
  // out serial tests.
  $parallel_threads = min(get_num_threads($options), \count($tests)) as int;
  foreach ($tests as $test) {
    if (in_array($test, $serial_tests)) continue;
    Status::$tests[] = $test;
  }

  // Remember that the serial tests are also in the tests array too,
  // so they are part of the total count.
  if (!$options->testpilot) {
    print "Running ".count($tests)." tests in ".
      $parallel_threads ." threads (" . count($serial_tests) .
      " in serial)\n";
  }

  if ($options->verbose) {
    Status::setMode(Status::MODE_VERBOSE);
  }
  if ($options->testpilot) {
    Status::setMode(Status::MODE_TESTPILOT);
  }
  if ($options->record_failures is nonnull) {
    Status::setMode(Status::MODE_RECORD_FAILURES);
  }
  Status::setUseColor($options->color || posix_isatty(HH\stdout()));

  Status::$nofork = count($tests) === 1 && $servers is nonnull;

  if (!Status::$nofork) {
    // Create the Queue before any children are forked.
    $queue = Status::getQueue();

    // Fork a "printer" child to process status messages.
    Status::$printer_pid = pcntl_fork();
    if (Status::$printer_pid === -1) {
      error("failed to fork");
    } else if (Status::$printer_pid === 0) {
      msg_loop(count($tests), $queue);
      return 0;
    }
  }

  // Unblock the Queue (if needed).
  Status::started();

  // Fork "worker" children (if needed).
  // We write results as json in each child and collate them at the end
  $json_results_files = vec[];
  if (Status::$nofork) {
    Status::registerCleanup($options->no_clean);
    $json_results_file = tempnam(Status::getWorkingDir(), 'test-run-');
    $json_results_files[] = $json_results_file;
    $return_value = child_main($options, $serial_tests, $json_results_file);
  } else {
    for ($i = 0; $i < $parallel_threads; ++$i) {
      $json_results_file = tempnam(Status::getWorkingDir(), 'test-run-');
      $json_results_files[] = $json_results_file;
      $pid = pcntl_fork();
      if ($pid === -1) {
        error('could not fork');
      } else if ($pid !== 0) {
        Status::$children[] = $pid;
      } else {
        $serial = ($i === 0) ? $serial_tests : vec[];
        exit(child_main($options, $serial, $json_results_file));
      }
    }

    // Make sure to clean up on exit, or on SIGTERM/SIGINT.
    // Do this here so no children inherit this.
    Status::registerCleanup($options->no_clean);

    // Have the parent wait for all forked children to exit.
    $return_value = 0;
    while (count(Status::$children) !== 0 && Status::$printer_pid !== 0) {
      $status = null;
      $pid = pcntl_wait(inout $status);
      if (pcntl_wifexited($status as nonnull)) {
        $bad_end = pcntl_wexitstatus($status) !== 0;
      } else if (pcntl_wifsignaled($status)) {
        $bad_end = true;
      } else {
        error("Unexpected exit status from child");
      }

      if ($pid === Status::$printer_pid) {
        // We should be finishing up soon.
        Status::$printer_pid = 0;
        if ($bad_end) {
          // Don't consider the run successful if the printer worker died
          $return_value = 1;
        }
      } else if ($servers is nonnull && isset($servers->pids[$pid])) {
        // A server crashed. Restart it.
        // We intentionally ignore $bad_end here because we expect this to
        // show up as a test failure in whatever test was running on the server
        // when it crashed. TODO(alexeyt): assert $bad_end === true?
        if (getenv('HHVM_TEST_SERVER_LOG')) {
          echo "\nServer $pid crashed. Restarting.\n";
        }
        Status::serverRestarted();
        $server = $servers->pids[$pid];
        $server = start_server_proc($options, $server->config, $server->port);

        // Unset the old $pid entry and insert the new one.
        unset($servers->pids[$pid]);
        $pid = $server->pid;
        $servers->pids[$pid] = $server;
      } else if (isset(Status::$children[$pid])) {
        unset(Status::$children[$pid]);
        if ($bad_end) {
          // If any worker process dies we should fail the test run
          $return_value = 1;
        }
      } else {
        // We definitely see messages about defunct processes that were started
        // on our behalf - and it's too late to ask about their name. Don't
        // error out because of that.
        //
        // A common example is: [scribe_cat] <defunct>.
        fprintf(
          HH\stderr(),
          "WARNING: Got status for child that we didn't know we had with pid $pid\n"
        );
      }
    }
  }

  Status::finished($return_value);

  // Wait for the printer child to exit, if needed.
  if (!Status::$nofork && Status::$printer_pid !== 0) {
    $status = 0;
    $pid = pcntl_waitpid(Status::$printer_pid, inout $status);
    $status = $status as int;
    Status::$printer_pid = 0;
    if (pcntl_wifexited($status)) {
      if (pcntl_wexitstatus($status) !== 0) {
        // Don't consider the run successful if the printer worker died
        $return_value = 1;
      }
    } else if (pcntl_wifsignaled($status)) {
      // Don't consider the run successful if the printer worker died
      $return_value = 1;
    } else {
      error("Unexpected exit status from child");
    }
  }

  // Kill the servers.
  if ($servers is nonnull) {
    foreach ($servers->pids as $server) {
      proc_terminate($server->proc);
      proc_close($server->proc);
    }
  }

  // Aggregate results.
  $results = dict[];
  foreach ($json_results_files as $json_results_file) {
    $contents = file_get_contents($json_results_file);
    $json = json_decode($contents, true);
    if (!is_dict($json)) {
      error(
        "\nNo JSON output was received from a test thread. ".
        "Either you killed it, or it might be a bug in the test script.",
      );
    }
    $results = array_merge($results, $json);
    unlink($json_results_file);
  }

  // Print results.
  if ($options->record_failures is nonnull) {
    $fail_file = $options->record_failures;
    $failed_tests = vec[];
    $prev_failing = vec[];
    if (file_exists($fail_file)) {
      $prev_failing = explode("\n", file_get_contents($fail_file));
    }

    $new_fails = 0;
    $new_passes = 0;
    foreach ($results as $r) {
      if (!isset($r['name']) || !isset($r['status'])) continue;
      $test = canonical_path($r['name']);
      $status = $r['status'];
      if ($status === 'passed' && in_array($test, $prev_failing)) {
        $new_passes++;
        continue;
      }
      if ($status !== 'failed') continue;
      if (!in_array($test, $prev_failing)) $new_fails++;
      $failed_tests[] = $test;
    }
    printf(
      "Recording %d tests as failing.\n".
      "There are %d new failing tests, and %d new passing tests.\n",
      count($failed_tests), $new_fails, $new_passes
    );
    sort(inout $failed_tests);
    file_put_contents($fail_file, implode("\n", $failed_tests));
  } else if ($options->testpilot) {
    Status::say(dict['op' => 'all_done', 'results' => $results]);
    return $return_value;
  } else if ($return_value === 0) {
    print_success($tests, $results, $options);
  } else {
    print_failure($argv, $results, $options);
  }

  $overall_time = Status::getOverallEndTime() - Status::getOverallStartTime();
  $serial_time = Status::addTestTimesSerial($results);

  $per_test = (count($tests) > 0) ? ($overall_time / count($tests)) : 0.0;
  $serial_per_test = (count($tests) > 0) ? ($serial_time / count($tests)) : 0.0;

  Status::sayColor("\nTotal time for all executed tests as run: ",
                   Status::BLUE,
                   sprintf("%.2fs (%.2fs/test)\n",
                           $overall_time, $per_test));
  Status::sayColor("Total time for all executed tests if run serially: ",
                   Status::BLUE,
                   sprintf("%.2fs (%.2fs/test)\n",
                           $serial_time, $serial_per_test));

  return $return_value;
}

<<__EntryPoint>>
function run_main(): void {
  exit(main(get_argv()));
}

function help(): string {
  $argv = get_argv();
  $ztestexample = 'test/zend/good/*/*z*.php'; // sep. for syntax highlighting.
  $help = <<<EOT


This is the hhvm test-suite runner.  For more detailed documentation,
see hphp/test/README.md.

The test argument may be a path to a php test file, a directory name, or
one of a few pre-defined suite names that this script knows about.

If you work with hhvm a lot, you might consider a bash alias:

   alias htest="path/to/hphp/test/run"

Examples:

  # Quick tests in JIT mode:
  % {$argv[0]} test/quick

  # Slow tests in interp mode:
  % {$argv[0]} -m interp test/slow

  # PHP specification tests in JIT mode:
  % {$argv[0]} test/slow/spec

  # Slow closure tests in JIT mode:
  % {$argv[0]} test/slow/closure

  # Slow closure tests in JIT mode with RepoAuthoritative:
  % {$argv[0]} -r test/slow/closure

  # Slow array tests, in RepoAuthoritative:
  % {$argv[0]} -r test/slow/array

  # Zend tests with a "z" in their name:
  % {$argv[0]} $ztestexample

  # Quick tests in JIT mode with some extra runtime options:
  % {$argv[0]} test/quick -a '-vEval.JitMaxTranslations=120 -vEval.HHIRRefcountOpts=0'

  # Quick tests in JIT mode with RepoAuthoritative and an extra compile-time option:
  % {$argv[0]} test/quick -r --compiler-args '--log=4'

  # All quick tests except debugger
  % {$argv[0]} -e debugger test/quick

  # All tests except those containing a string of 3 digits
  % {$argv[0]} -E '/\d{3}/' all

  # All tests whose name containing pdo_mysql
  % {$argv[0]} -i pdo_mysql -m jit -r zend

  # Print all the standard tests
  % {$argv[0]} --list-tests

  # Use a specific HHVM binary
  % {$argv[0]} -b ~/code/hhvm/hphp/hhvm/hhvm
  % {$argv[0]} --hhvm-binary-path ~/code/hhvm/hphp/hhvm/hhvm

  # Use retranslate all.  Run the test n times, then run retranslate all, then
  # run the test n more on the new code.
  % {$argv[0]} --retranslate-all 2 quick

  # Use jit-serialize.  Run the test n times, then run retranslate all, run the
  # test once more, serialize all profile data, and then restart hhvm, load the
  # serialized state and run retranslate-all before starting the test.
  % {$argv[0]} --jit-serialize  2 -r quick
EOT;

  return usage().$help;
}

// Inline ASCII art moved to end-of-file to avoid confusing emacs.

function print_clown(): void {
  print <<<CLOWN
            _
           {_}
           /*\\
          /_*_\\
         {('o')}
      C{{([^*^])}}D
          [ * ]
         /  Y  \\
        _\\__|__/_
       (___/ \\___)
CLOWN
        ."\n\n";
}

function print_skipper(): void {
  print <<<SKIPPER
                          .".
                         /  |
                        /  /
                       / ,"
           .-------.--- /
          "._ __.-/ o. o\
             "   (    Y  )
                  )     /
                 /     (
                /       Y
            .-"         |
           /  _     \    \
          /    `. ". ) /' )
         Y       )( / /(,/
        ,|      /     )
       ( |     /     /
        " \_  (__   (__
            "-._,)--._,)
SKIPPER
        ."\n\n";
}

function print_ship(): void {
  print <<<SHIP
              |    |    |
             )_)  )_)  )_)
            )___))___))___)\
           )____)____)_____)\\
         _____|____|____|____\\\__
---------\      SHIP IT      /---------
  ^^^^^ ^^^^^^^^^^^^^^^^^^^^^
    ^^^^      ^^^^     ^^^    ^^
         ^^^^      ^^^
SHIP
      ."\n";
}
