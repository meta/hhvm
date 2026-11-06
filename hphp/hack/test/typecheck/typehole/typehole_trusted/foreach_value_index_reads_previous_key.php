<?hh

final class C {
  public dict<int, string> $out = dict[];

  public function fill(dict<int, string> $d): void {
    $k = 'not a key';
    foreach ($d as $k => $this->out[$k]) {}
  }
}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  $c = new C();
  $c->fill(dict[1 => 'v']);
  foreach ($c->out as $key => $_) {
    takes_int($key);
  }
}
