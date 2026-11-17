<?hh

<<__Memoize>>
function optional_default(
  int $value,
  int $offset = 7,
): vec<int> {
  echo "compute optional default\n";
  return vec[$value, $offset];
}

<<__Memoize>>
function many_defaults(
  int $value,
  int $a = 1,
  int $b = 2,
  int $c = 3,
  int $d = 4,
  int $e = 5,
  int $f = 6,
  int $g = 7,
  int $h = 8,
  int $i = 9,
  int $j = 10,
  int $k = 11,
): int {
  echo "compute many defaults\n";
  return $value + $a + $b + $c + $d + $e + $f + $g + $h + $i + $j + $k;
}

<<__EntryPoint>>
function main(): void {
  for ($i = 0; $i < 3; $i++) {
    var_dump(optional_default(42));
  }
  for ($i = 0; $i < 3; $i++) {
    var_dump(many_defaults(42));
  }
}
