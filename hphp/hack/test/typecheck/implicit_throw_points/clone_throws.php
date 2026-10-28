<?hh

final class Thrower {
  public function __clone(): void {
    throw new Exception('clone');
  }
}

// __clone() can throw, so the catch sees $x as string.
function clone_throws(Thrower $thrower): int {
  $x = 0;
  try {
    $x = 'foo';
    $_copy = clone $thrower;
    $x = 1;
  } catch (Exception $_) {
    return $x;
  }
  return $x;
}

function clone_ok(Thrower $thrower): int {
  $x = 0;
  try {
    $_copy = clone $thrower;
    $x = 1;
  } catch (Exception $_) {
    return $x;
  }
  return $x;
}
