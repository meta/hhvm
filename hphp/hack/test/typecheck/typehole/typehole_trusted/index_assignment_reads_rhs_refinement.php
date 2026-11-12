<?hh

class C {
  public arraykey $i = 0;

  public function invalidate(): int {
    $this->i = '42';
    return 0;
  }
}

function takes_string(string $_): void {}

<<__EntryPoint>>
function main(): void {
  $c = new C();
  $d = dict<string, mixed>[];
  $d[$c->i] = tuple($c->invalidate(), $c->i as string);
  foreach ($d as $k => $_) {
    takes_string($k);
  }
}
