<?hh

// list() throws on a short vec, so the catch sees $x as string.
function list_throws(): int {
  $x = 0;
  try {
    $x = 'foo';
    list($_a, $_b) = vec[1];
    $x = 1;
  } catch (Exception $_) {
    return $x;
  }
  return $x;
}

function list_ok(vec<int> $v): int {
  $x = 0;
  try {
    list($a, $b) = $v;
    $x = $a + $b;
  } catch (Exception $_) {
    return $x;
  }
  return $x;
}

function list_outside_try(): int {
  list($a, $b) = tuple(1, 2);
  return $a + $b;
}
