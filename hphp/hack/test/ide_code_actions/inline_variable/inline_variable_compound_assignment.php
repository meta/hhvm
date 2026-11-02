<?hh

function compound_assignment(): void {
  $x = 10;
  $x += 2;
  var_dump(/*range-start*/$x/*range-end*/);
}
