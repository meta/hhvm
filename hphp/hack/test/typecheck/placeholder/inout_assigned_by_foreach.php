<?hh

function foo(inout int $_): void {
  foreach (vec['str'] as $_) {}
}

<<__EntryPoint>>
function main(): void {
  $x = 0;
  foo(inout $x);
}
