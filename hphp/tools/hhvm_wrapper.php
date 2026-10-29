#!/usr/local/bin/php
<?hh

//////////////////////////////////////////////////////////////////////

function my_option_map(): OptionInfoMap {
  return Map {
'help'            => Pair { 'h', 'Print help message' },
'bin:'            => Pair { 'b', 'Use a specific HHVM binary' },
'gdb'             => Pair { 'g', 'Run the whole command in gdb' },
'lldb'            => Pair { 'l', 'Run the whole command in lldb' },
'server'          => Pair { '',  'Run a server, port 80, pwd as the root' },
'interp'          => Pair { 'i', 'Disable the JIT compiler' },
'opt-ir'          => Pair { 'o', 'Disable debug assertions in IR output' },
'dump-tc'         => Pair { 'j', 'Dump the contents of the translation cache' },
'dump-hhbc'       => Pair { 'v', 'Print the HHBC for units after compiling' },
'dump-hhas'       => Pair { '',  'Print the HHAS after compiling, then exit' },
'trace-printir::4'=> Pair { 'p', 'Extend TRACE with printir=n' },
'trace-hhir::3'   => Pair { 't', 'Extend TRACE with hhir=n' },
'trace-file'      => Pair { 'f', 'Dump trace in file $HPHP_TRACE_FILE' },
'hphpd'           => Pair { 'd', 'Run as the hphpd client' },
'compile'         => Pair { 'c', 'Compile with hphpc; run RepoAuthoritative' },
'create-repo'     => Pair { 'C', 'Compile unoptimized repo named hhvm.hhbc' },
'repo:'           => Pair { 'R', 'Use an already-compiled repo' },
'retranslate-all:'=> Pair { 'r', 'Emit optimized code after n profiling runs' },
'php7'            => Pair { '7', 'Enable PHP7 mode' },
'jit-gdb'         => Pair { '',  'Enable JIT symbols in GDB' },
'jit-serialize'   => Pair { 's', 'Jumpstart after doing retranslate-all' },
'print-command'   => Pair { '',  'Just print the command, don\'t run it' },
'region-mode:'    => Pair { '',
                            'Which region selector to use (e.g \'method\')' },
'no-pgo'          => Pair { '',  'Disable PGO' },
'bespoke:'        => Pair { '',  'Bespoke array mode' },
'hadva'           => Pair { '',  'Enable HAM and automarking' },
'pgo-threshold:'  => Pair { '',  'PGO threshold to use' },
'no-obj-destruct' => Pair { '',
                            'Disable global object destructors in CLI mode' },
'arm'             => Pair { '',  'Emit ARM code and simulate it' },
'ini[]'           => Pair { '',  'An .ini configuration file or CLI option' },
'hdf[]'           => Pair { '',  'An .hdf configuration file or CLI option' },
'no-defaults'     => Pair { '',
                            'Do not use the default wrapper runtime options'},
'perf:'           => Pair { '', 'Run perf record'},
'allow-unstable-features'   => Pair { 'u',  'Allow running unstable features' },
  };
}

function get_hhvm_path(OptionMap $opts): string {
  if ($opts->containsKey('bin')) {
    return $opts['bin'];
  }

  $buck = __DIR__.'/../../buck-out/gen/hphp/hhvm/hhvm/hhvm';
  $buck2 = __DIR__.'/../../../buck-out/v2/gen/fbcode/hphp/hhvm/out/hhvm';

  $bins = vec[$buck, $buck2] |> HH\Lib\Vec\filter($$, $bin ==> file_exists($bin));

  if (HH\Lib\C\is_empty($bins)) {
    echo "Couldn't find an HHVM binary in the following locations:\n";
    echo " - " . $buck . "\n";
    echo " - " . $buck2 . "\n";
    error("Build HHVM first.");
  } else if (HH\Lib\C\count($bins) > 1) {
    echo "Multiple HHVM binaries found:\n";
    foreach ($bins as $bin) {
      echo " - " . $bin . "\n";
    }
    error("Delete one of them or use the --bin flag.");
  }
  return $bins[0];
}

function determine_flags(OptionMap $opts): string {
  $flags = '';
  $has_file = false;

  if ($opts->containsKey('ini')) {
    $ret = parse_config_options($opts['ini'], "ini");
    $flags .= $ret[0];
    $has_file = $ret[1];
  }
  if ($opts->containsKey('hdf')) {
    $ret = parse_config_options($opts['hdf'], "hdf");
    $flags .= $ret[0];
    // Don't override if we have an ini file already
    $has_file = $has_file || $ret[1];
  }
  // If no config files were given at the command line, use a default
  if (!$has_file) {
    // The cli.hdf file is where Facebook puts its in-house
    // default configuration information.
    $facebook_cli_config_file_name = '/usr/local/hphpi/cli.hdf';
    if (file_exists($facebook_cli_config_file_name)) {
      $flags .= "-c $facebook_cli_config_file_name ";
    }
  }

  // Switch to single-threaded mode when tracing to avoid mixing traces.
  if (determine_trace_env($opts)) {
    $flags .=
      '-v Eval.JitThreads=1 '.
      '-v Eval.JitWorkerThreads=1 '.
      '';
  }

  if (!$opts->containsKey('no-defaults')) {
    $flags .=
      '-v Eval.JitEnableRenameFunction=0 '.
      '-v Eval.GdbSyncChunks=1 '.
      '-v Eval.AllowHhas=true '.
      '';
  }

  if ($opts->containsKey('interp')) {
    $flags .=
      '-v Eval.Jit=0 '.
      '';
  } else {
    $flags .=
      '-v Eval.Jit=1 '.
      '';
  }

  if ($opts->containsKey('jit-serialize')) {
    if (!$opts->containsKey('compile') && !$opts->containsKey('repo')) {
      error('jit-serialize requires that you specify a repo.');
    }
    if ($opts->containsKey('region-mode') && $opts['region-mode'] === 'method') {
      error('jit-serialize option is not compatible with region-mode==method');
    }
    if (!$opts->containsKey('retranslate-all')) {
      $opts['retranslate-all'] = 1;
    }
  }

  if ($opts->containsKey('retranslate-all')) {
    $times = (int)$opts['retranslate-all'];
    $flags .=
        '--count='.(2 * $times).' '.
        '-v Eval.JitRetranslateAllRequest='.$times.' '.
        '-v Eval.JitRetranslateAllSeconds=300 '.
        '';
  }

  if ($opts->containsKey('bespoke')) {
    $mode = (int)$opts['bespoke'];
    $flags .=
        '-v Eval.BespokeArrayLikeMode='.$mode.' '.
        '-v Eval.ExportLoggingArrayDataPath="/tmp/logging-array-export" '.
        '-v Eval.EmitLoggingArraySampleRate=17 '.
        '-v Eval.BespokeEscalationSampleRate=1000 '.
        '';
  }

  if ($opts->containsKey('region-mode')) {
    if ($opts['region-mode'] == 'method') {
      $flags .=
        '-v Eval.JitPGO=0 '.
        '';
      if (!$opts->containsKey('compile')) {
        echo 'Reminder: running region-mode=method without --compile is '.
             "almost never going to work...\n";
      }
    }
    if ($opts['region-mode'] == 'wholecfg') {
      $flags .=
        '-v Eval.JitPGORegionSelector='.((string)$opts['region-mode']).' '.
        '';
    } else {
      $flags .=
        '-v Eval.JitRegionSelector='.((string)$opts['region-mode']).' '.
        '';
    }
  }

  $simple_args = Map {
    'dump-hhbc'       => '-v Eval.DumpBytecode=1 ',
    'dump-hhas'       => '-v Eval.DumpHhas=1 ',
    'dump-tc'         => '-v Eval.DumpTC=1 ',
    'php7'            => '-d hhvm.php7.all=1 ',
    'opt-ir'          => '-v Eval.HHIRGenerateAsserts=0 ',
    'jit-gdb'         => '-v Eval.JitNoGdb=false ',
    'no-pgo'          => '-v Eval.JitPGO=false ',
    'hphpd'           => '-m debug ',
    'server'          => '-m server ',
    'allow-unstable-features' => '-v Hack.Lang.AllowUnstableFeatures=1 ',
  };

  if ($opts->containsKey('pgo-threshold')) {
    $flags .=
      '-v Eval.JitPGOThreshold='.((string)$opts['pgo-threshold']).' '.
      '';
  }

  foreach ($simple_args as $k => $v) {
    if ($opts->containsKey($k)) {
      $flags .= $v;
    }
  }

  return $flags;
}

function parse_config_options(Set $options, string $kind): Pair<string, bool> {
  $flags = "";
  $has_file = false;
  foreach ($options as $option) {
    if (file_exists($option)) {
      $flags .= "-c " . $option . " ";
      $has_file = true;
    } else {
      $dashwhat = $kind === "ini" ? "-d " : "-v ";
      $flags .= $dashwhat . $option . " ";
    }
  }
  return Pair {$flags, $has_file};
}

// If we're tracing, colorize the trace and print it to stdout.
// If we're not tracing, the result will be empty.
function determine_trace_env(OptionMap $opts): string {
  $trace_opts = Map {
    'trace-hhir'     => 'hhir',
    'trace-printir'  => 'printir',
  };

  $traces = Vector {};
  foreach ($trace_opts as $k => $v) {
    if ($opts->containsKey($k)) {
      $traces->add("$v:".((string)$opts[$k]));
    }
  }

  $env = getenv("TRACE");
  if ($traces->isEmpty() && !$env) return '';

  $formatting = '';
  if (!$opts->containsKey('trace-file')) {
    $formatting = 'HPHP_TRACE_FILE=/dev/stdout HPHP_TRACE_TTY=1 ';
  }
  if (!$traces->isEmpty() && $env) $env .= ',';
  return $formatting.'TRACE='.(string)$env.implode(',', vec($traces)).' ';
}

function argv_for_shell(): string {
  $ret = '';
  foreach (\HH\global_get('argv') as $arg) {
    $ret .= '"'.$arg.'" ';
  }
  return $ret;
}

function compile_a_repo(bool $unoptimized, OptionMap $opts): string {
  $echo_command = $opts->containsKey('print-command');
  $runtime_flags = determine_flags($opts);
  $hphpc_flags = $runtime_flags
    |> preg_replace("/-v\s*/", "-vRuntime.", $$)
    |> preg_replace("/--count=[0-9]+\s*/", "", $$);
  $hphp_out='/tmp/hphp_out'.posix_getpid();

  $output_dir='/tmp/hphp_'.posix_getpid();
  $hphpc_flags .=
    '--output-dir='.$output_dir.' '.
    '';

  $cmd = get_hhvm_path($opts).' '.
    '--hphp '.
    ($unoptimized ? '-v UseHHBBC=0 ' : '').
    ($opts->containsKey('php7') ? '-d hhvm.php7.all=1 ' : '').
    '-l3 '.
    $hphpc_flags.
    argv_for_shell().
    " >$hphp_out 2>&1";
  if ($echo_command) {
    echo "\n", $cmd, "\n";
  }
  $compile_rv = -1;
  system($cmd, inout $compile_rv);

  $repo=$output_dir.'/hhvm.hhbc';
  $return_var = -1;
  system("rm -f $hphp_out", inout $return_var);
  if ($echo_command !== true) {
    register_shutdown_function(
      function() use ($output_dir) {
        $return_var = -1;
        system("rm -fr $output_dir", inout $return_var);
      },
    );
    if ($compile_rv !== 0) {
      exit($compile_rv);
    }
  }

  return $repo;
}

function repo_auth_flags(string $flags, string $repo): string {
  return $flags .
    '-v Repo.Authoritative=true '.
    "-v Repo.Path=$repo ";
}

function compile_with_hphp(string $flags, OptionMap $opts): string {
  return repo_auth_flags($flags, compile_a_repo(false, $opts));
}

function create_repo(OptionMap $opts): void {
  $repo = compile_a_repo(true, $opts);
  $return_var = -1;
  system("cp $repo ./hhvm.hhbc", inout $return_var);
}

function do_jumpstart(string $flags, OptionMap $opts): string {
  $hhvm = get_hhvm_path($opts);
  $prof = '/tmp/jit.prof.'.posix_getpid();
  $requests = $opts->get('jit-serialize');
  $filename = argv_for_shell();
  if (strlen($filename) === 0) {
    throw new Error('Jumpstart expects a file!');
  }
  $cmd = "$hhvm $flags --file $filename"
    ." -v Eval.JitSerdesFile=$prof"
    .' -v Eval.JitSerdesMode=SerializeAndExit'
    .' >/dev/null 2>&1';

  if ($opts->containsKey('print-command')) {
    echo "\n", $cmd, "\n";
  }
  $code = -1;
  system($cmd, inout $code);
  if ($code != 0) throw new Error('Jumpstart failed!');
  return $flags
    ." -v Eval.JitSerdesFile=$prof"
    .' -v Eval.JitSerdesMode=DeserializeOrFail';
}

function run_hhvm(OptionMap $opts): void {
  $flags = determine_flags($opts);
  if ($opts->containsKey('create-repo')) {
    create_repo($opts);
    exit(0);
  }
  if ($opts->containsKey('gdb') && $opts->containsKey('lldb')) {
      error('Can only specify a single debugger');
  }
  if ($opts->containsKey('repo')) {
    $flags = repo_auth_flags($flags, (string) $opts['repo']);
  } else if ($opts->containsKey('compile')) {
    $flags = compile_with_hphp($flags, $opts);
  }

  if ($opts->containsKey('jit-serialize')) {
    $flags = do_jumpstart($flags, $opts);
  }

  $pfx = determine_trace_env($opts);
  if ($opts->containsKey('gdb')) {
    $pfx .= 'gdb --args ';
  } else if ($opts->containsKey('lldb')) {
    $pfx .= 'lldb -- ';
  }
  if ($opts->containsKey('perf')) {
    $pfx .= 'perf record -g -o ' . $opts['perf'] . ' ';
  }
  $hhvm = get_hhvm_path($opts);
  $filename = argv_for_shell();
  if (strlen($filename) === 0) {
    $cmd = "$pfx $hhvm $flags";
  } else {
    $cmd = "$pfx $hhvm $flags --file $filename";
  }
  if ($opts->containsKey('gdb') || $opts->containsKey('lldb')) {
    // Trick gdb into thinking gdb_stderr->isatty(), so tui mode can be used.
    // Also enables lldb to progress after trying to set the target.run-args.
    $cmd = "script -q --return -c \" $cmd \" /dev/null";
  }
  if ($opts->containsKey('print-command')) {
    echo "\n$cmd\n\n";
  } else {
    // Give the return value of the command back to the caller.
    $retval = null;
    passthru($cmd, inout $retval);
    exit($retval);
  }
}

function ends_with(string $haystack, string $needle): bool {
  return $needle === "" || substr($haystack, -strlen($needle)) === $needle;
}

<<__EntryPoint>>
function main(): void {
  require_once __DIR__.'/command_line_lib.php';
  $opts = parse_options(my_option_map());
  if ($opts->containsKey('help')) {
    help();
    return;
  }
  run_hhvm($opts);
}

//////////////////////////////////////////////////////////////////////

function help(): void {
  display_help(
"hhvm_wrapper\n".
"\n".
"   This script is a wrapper for hhvm and hphpc with command line\n".
"   flags that are a little more terse and geared toward common\n".
"   debugging/development tasks.  (Basically shorthands for various\n".
"   common combinations of RuntimeOption things.)\n".
"\n".
"   You might consider adding a bash alias:\n".
"\n".
"      #!/bin/bash\n".
"      hphp/tools/hhvm_wrapper.php \"$@\"\n".
"\n".
"   Note: this alias will only work from repo root. That's intended,\n".
"   as that's where the script will look for a compiled HHVM binary.\n",
    my_option_map(),
  );

  echo
"Usage Examples:\n".
"\n".
"   # Run with the bytecode interpreter\n".
"   % hhvm -i test.php\n".
"\n".
"   # Run using the JIT:\n".
"   % hhvm test.php\n".
"\n".
"   # Dump the bytecode, then run:\n".
"   % hhvm -v test.php\n".
"\n".
"   # Compile repo with hphpc and run as repo authoritative, using the\n".
"   # JIT, with tracing.\n".
"   % TRACE=hhir:2 hhvm --compile -v test.php\n".
"\n".
"   # Same thing, but run it under gdb:\n".
"   % TRACE=hhir:2 hhvm -g --compile test.php\n".
"\n".
"   # Compile a repo, optimize it with hhbbc, then run using the output:\n".
"   % hhvm -C test.php  # creates hhvm.hhbc\n".
"   % hhbbc             # creates hhvm.hhbbc\n".
"   % hhvm --repo hhvm.hhbbc test.php\n".
"\n".
"   # Specify a configuration file to be used when running your code:\n".
"   % hhvm --ini test.ini test.php\n".
"\n".
"   # Specify multiple config files to be used when running your code:\n".
"   % hhvm --ini a.ini --hdf b.hdf --ini c.ini test.php\n".
"\n".
"   # Specify config option(s) to be used when running your code:\n".
"   % hhvm --ini hhvm.jit_mature_size=15728640 test.php\n".
"\n"
    ;
}

//////////////////////////////////////////////////////////////////////
