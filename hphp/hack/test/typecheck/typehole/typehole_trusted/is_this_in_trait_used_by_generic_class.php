<?hh

// `this` in a trait is the using class, so `is this` in a non-generic trait
// refines to the generic user's own instantiation, which is never checked.
trait IsThisTrait {
  public function rewrap(mixed $x): ?this {
    if ($x is this) {
      return $x;
    }
    return null;
  }
}

final class IsThisBox<T> {
  use IsThisTrait;

  public function __construct(public T $item) {}
}

function expect_string(string $_): void {}

<<__EntryPoint>>
function break_it(): void {
  $box = new IsThisBox('a');
  $rewrapped = $box->rewrap(new IsThisBox(3));
  if ($rewrapped is nonnull) {
    expect_string($rewrapped->item);
  }
}
