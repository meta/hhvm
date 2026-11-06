<?hh

function takes_int(int $_): void {}

function destructure_each(vec<(string, int)> $v): void {
  foreach ($v as list($a, $a)) {
    takes_int($a);
  }
}

<<__EntryPoint>>
function main(): void {
  destructure_each(vec[tuple('str', 1)]);
}
