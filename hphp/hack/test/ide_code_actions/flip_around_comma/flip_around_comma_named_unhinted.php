<?hh

function f(): void {
  $callback = (named $a, /*range-start*//*range-end*/named $b) ==> tuple($a, $b);
}
