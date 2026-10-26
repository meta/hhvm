<?hh

function result(bool $value): bool {
  $x = $value ?: true;
  return !/*range-start*/$x/*range-end*/;
}

<<__EntryPoint>>
function main(): void {
  var_dump(result(true), result(false));
}
