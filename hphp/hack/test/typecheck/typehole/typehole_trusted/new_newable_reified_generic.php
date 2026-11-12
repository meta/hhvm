<?hh

<<__ConsistentConstruct>>
abstract class Inv<T> {
  public function __construct(private T $x) {}

  public function get(): T {
    return $this->x;
  }
}

final class IntInv extends Inv<int> {}

function make<<<__Newable>> reify TC as Inv<int>>(): TC {
  return new TC('str');
}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  takes_int(make<IntInv>()->get());
}
