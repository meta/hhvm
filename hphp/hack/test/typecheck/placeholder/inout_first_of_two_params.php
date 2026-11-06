<?hh

function foo(inout int $_, inout string $_): void {
  $_ = 'str';
}

<<__EntryPoint>>
function main(): void {
  $x = 0;
  $y = '';
  foo(inout $x, inout $y);
}
