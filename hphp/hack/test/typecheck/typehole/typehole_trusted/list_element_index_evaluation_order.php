<?hh

class C {
  public arraykey $i = 0;

  public function invalidate(): int {
    $this->i = '42';
    return 0;
  }
}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  $c = new C();
  $v = vec[0];
  if ($c->i is int) {
    list($v[$c->invalidate()]) = vec[$c->i];
    takes_int($v[0]);
  }
}
