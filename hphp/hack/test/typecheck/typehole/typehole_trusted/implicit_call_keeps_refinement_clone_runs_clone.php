<?hh

final class Holder {
  public ?int $x = 1;
}

final class Clearer {
  public function __construct(private Holder $holder) {}
  public function __clone(): void {
    $this->holder->x = null;
  }
}

final class PureClone {
  public function __clone()[]: void {}
}

final class NoClone {}

// __clone() runs user code, which clears $holder->x.
function clone_clearer(Holder $holder, Clearer $clearer): int {
  if ($holder->x is nonnull) {
    $_copy = clone $clearer;
    return $holder->x;
  }
  return 0;
}

// A pure __clone() can't write properties, so the refinement stands.
function clone_pure(Holder $holder, PureClone $c): int {
  if ($holder->x is nonnull) {
    $_copy = clone $c;
    return $holder->x;
  }
  return 0;
}

function clone_no_clone(Holder $holder, NoClone $c): int {
  if ($holder->x is nonnull) {
    $_copy = clone $c;
    return $holder->x;
  }
  return 0;
}
