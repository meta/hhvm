<?hh

function compound_assignment_then_reset(): void {
  $x = 10;
  $x += 2;
  $x = 3;
  var_dump(/*range-start*/$x/*range-end*/);
}
