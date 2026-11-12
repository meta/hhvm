<?hh

abstract class Base {
  <<__Enforceable>>
  abstract const type TSelf as Base;

  public function cast(mixed $m): this::TSelf {
    return $m as this::TSelf;
  }
}

final class Box<T> extends Base {
  const type TSelf = this;

  public function __construct(public T $item) {}
}

function expect_string(string $_): void {}

<<__EntryPoint>>
function main(): void {
  $box = new Box('a');
  expect_string($box->cast(new Box(1))->item);
}
