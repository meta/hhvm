<?hh

function result(bool $condition): int {
  $x = $condition ? 1 : 2;
  return /*range-start*/$x/*range-end*/ + 3;
}

<<__EntryPoint>>
function main(): void {
  var_dump(result(true), result(false));
}
