<?hh

function compound_coalesce(?int $initial = 10): void {
  $x = $initial;
  $x ??= 2;
  var_dump(/*range-start*/$x/*range-end*/);
}
