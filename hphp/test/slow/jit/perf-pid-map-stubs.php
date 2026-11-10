<?hh

<<__EntryPoint>>
function main(): void {
  $map = file_get_contents('/tmp/perf-'.getmypid().'.map');
  if ($map === false) {
    throw new Exception('Could not read the perf map');
  }

  foreach (vec[
    'callToExit',
    'retHelper',
    'genRetHelper',
    'asyncGenRetHelper',
  ] as $stub) {
    echo $stub.': ';
    var_dump(preg_match('/^[0-9a-f]+ [0-9a-f]+ HHVM::'.$stub.'$/m', $map) === 1);
  }
}
