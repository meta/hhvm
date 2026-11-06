<?hh

enum EInt: int as int {
  ONE = 1;
}

function takes_int(int $_): void {}

function f(mixed $m): void {
  if ($m is EInt) {
    takes_int($m);
  }
}

<<__EntryPoint>>
function main(): void {
  f('1');
}
