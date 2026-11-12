<?hh

<<__ConsistentConstruct>>
abstract class A<+T> {
  public function __construct(private T $x) {}

  <<__NeedsConcrete>>
  final public static function make(T $x): this {
    return new static($x);
  }

  public function get(): T {
    return $this->x;
  }
}

final class B extends A<int> {}

function make_num(class<A<num>> $a): A<num> {
  return $a::make(1.1);
}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  $b = make_num(B::class) as B;
  takes_int($b->get());
}
