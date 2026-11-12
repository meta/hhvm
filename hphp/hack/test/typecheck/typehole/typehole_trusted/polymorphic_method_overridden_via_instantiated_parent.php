<?hh

abstract class B<TX> {
  abstract public function f<T>(T $x, TX $y): void;
}

class C<T> extends B<int> {
  public function __construct(public T $x) {}

  public function f(T $x, int $y): void {
    $this->x = $x;
  }
}

function call_through_parent(B<int> $b): void {
  $b->f('string', 1);
}

<<__EntryPoint>>
function main(): int {
  $c = new C(10);
  call_through_parent($c);
  return $c->x;
}
