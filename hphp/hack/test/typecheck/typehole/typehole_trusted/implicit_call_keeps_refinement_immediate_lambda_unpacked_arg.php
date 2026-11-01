<?hh

final class Holder {
  public ?int $x = 1;
}

function clear(Holder $holder): () {
  $holder->x = null;
  return tuple();
}

// The spread argument runs clear() before the lambda body.
function spread_clears(Holder $holder): int {
  if ($holder->x is nonnull) {
    return (() ==> $holder->x)(...clear($holder));
  }
  return 0;
}

// Spreading a local runs no code, so the refinement is kept.
function spread_local(Holder $holder, () $t): int {
  if ($holder->x is nonnull) {
    return (() ==> $holder->x)(...$t);
  }
  return 0;
}

function no_spread(Holder $holder): int {
  if ($holder->x is nonnull) {
    return ((int $a) ==> $holder->x + $a)(1);
  }
  return 0;
}
