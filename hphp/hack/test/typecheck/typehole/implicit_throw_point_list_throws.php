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
