<?hh

function foo(inout int $_): void {
  list($_, $_) = tuple('str', 1);
}

<<__EntryPoint>>
function main(): void {
  $x = 0;
  foo(inout $x);
}
