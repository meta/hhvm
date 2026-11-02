<?hh

function compound_assignment_dependency(): void {
  $y = 10;
  $x = $y;
  $y += 2;
  var_dump(/*range-start*/$x/*range-end*/);
}
