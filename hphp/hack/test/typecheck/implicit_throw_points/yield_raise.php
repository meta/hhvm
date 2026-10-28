<?hh

// Generator::raise() throws at the yield, so the catch sees $x as string.
function yield_raise(): Generator<int, int, void> {
  $x = 0;
  try {
    $x = 'foo';
    yield 1;
  } catch (Exception $_) {
    yield $x;
  }
}

function yield_ok(): Generator<int, int, void> {
  $x = 0;
  try {
    $x = 1;
    yield 1;
    $x = 2;
  } catch (Exception $_) {
    yield $x;
  }
}
