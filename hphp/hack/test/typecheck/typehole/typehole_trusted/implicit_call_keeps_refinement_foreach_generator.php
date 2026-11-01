<?hh

final class Holder {
  public ?int $x = 1;
}

function clearing(Holder $holder): Generator<int, int, void> {
  $holder->x = null;
  yield 0;
}

// Resuming the generator runs user code, which clears $holder->x.
function foreach_generator(Holder $holder): int {
  $gen = clearing($holder);
  if ($holder->x is nonnull) {
    foreach ($gen as $_) {
      return $holder->x;
    }
  }
  return 0;
}

// The generator may have run to completion when the loop exits.
function foreach_generator_after_loop(Holder $holder): int {
  $gen = clearing($holder);
  if ($holder->x is nonnull) {
    foreach ($gen as $_) {
    }
    return $holder->x;
  }
  return 0;
}

// Iterating arrays and collections runs no user code.
function foreach_containers(
  Holder $holder,
  vec<int> $v,
  dict<string, int> $d,
  Vector<int> $vector,
  Map<string, int> $map,
): int {
  if ($holder->x is nonnull) {
    foreach ($v as $_) {
      $_ = $holder->x + 1;
    }
    foreach ($d as $_) {
      $_ = $holder->x + 1;
    }
    foreach ($vector as $_) {
      $_ = $holder->x + 1;
    }
    foreach ($map as $_) {
      $_ = $holder->x + 1;
    }
    return $holder->x;
  }
  return 0;
}

// Refinements made inside the body still hold until the next iteration.
function refine_in_body(Holder $holder, Traversable<int> $t): int {
  foreach ($t as $i) {
    if ($holder->x is nonnull) {
      return $holder->x + $i;
    }
  }
  return 0;
}
