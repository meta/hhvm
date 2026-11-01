<?hh

<<__ConsistentConstruct>>
abstract class A<+T> {
  public function __construct(private T $x) {}
  public function get(): T {
    return $this->x;
  }
}
class B extends A<int> {}

function make_a_num(class<A<num>> $a): A<num> {
  return new $a(1.1);
}

function breakit(): int {
  $b = make_a_num(B::class) as B;
  return $b->get();
}

<<__EntryPoint>>
function main():void {
  breakit();
}
