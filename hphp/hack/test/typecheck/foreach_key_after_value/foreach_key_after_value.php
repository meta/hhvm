<?hh

function takes_int(int $_): void {}

function key_and_value_same_local(dict<string, int> $d): void {
  foreach ($d as $a => $a) {
    takes_int($a);
  }
}

function key_and_list_element_same_local(dict<string, vec<int>> $d): void {
  foreach ($d as $a => list($a, $_)) {
    takes_int($a);
  }
}

final class C {
  public dict<int, string> $out = dict[];

  public function value_index_reads_previous_key(dict<int, string> $d): void {
    $k = 'not a key';
    foreach ($d as $k => $this->out[$k]) {}
  }
}
