<?hh

interface I {
  public function f<T>(T $x): void;
}

class C<T> implements I {
  public function __construct(public T $x) {}
  public function f(T $x): void {
    $this->x = $x;
  }
}

class CInt extends C<int> {}

function upcast_to_i(I $i): void {
  $i->f('string');
}

<<__EntryPoint>>
function breakit(): int {
  $c = new CInt(10);
  upcast_to_i($c);
  return $c->x;
}
