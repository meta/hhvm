<?hh

function takes_int(int $_): void {}

function takes_string(string $_): void {}

function distinct_locals(dict<string, int> $d): void {
  foreach ($d as $k => $v) {
    takes_string($k);
    takes_int($v);
  }
}

function key_written_last(dict<int, string> $d): void {
  foreach ($d as $a => $a) {
    takes_int($a);
  }
}

final class C {
  public dict<int, string> $out = dict[];

  public function value_index_reads_previous_key(dict<int, string> $d): void {
    $k = 0;
    foreach ($d as $k => $this->out[$k]) {}
  }
}

async function value_written_last_under_await(
  AsyncKeyedIterator<string, int> $it,
): Awaitable<void> {
  foreach ($it await as $a => $a) {
    takes_int($a);
  }
}
