<?hh

// `this` in a trait is the using class, so `as this` in a non-generic trait
// refines to the generic user's own instantiation, which is never checked.
trait AsThisTrait {
  public function rewrap(): this {
    return (new AsThisBox<int>(3)) as this;
  }
}

final class AsThisBox<T> {
  use AsThisTrait;

  public function __construct(public T $item) {}
}

function expect_string(string $_): void {}

<<__EntryPoint>>
function break_it(): void {
  $box = new AsThisBox('a');
  expect_string($box->rewrap()->item);
}
