<?hh

final class Box<reify T> {}

type IntVec = vec<int>;

// The alias is validated as Box's argument, then skipped where `as` enforces it
function first_int(mixed $x): int {
  $t = $x as (Box<IntVec>, IntVec);
  return $t[1][0];
}

<<__EntryPoint>>
function main(): void {
  first_int(tuple(new Box<IntVec>(), vec['not an int']));
}
