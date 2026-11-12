<?hh

class C {
  public arraykey $i = 0;
  public int $j = 0;

  public function invalidate(): this {
    $this->i = '42';
    return $this;
  }
}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  $c = new C();
  if ($c->i is int) {
    $c->invalidate()->j = $c->i;
    takes_int($c->j);
  }
}
