<?hh
// @no-wrapper
<<file: __EnableUnstableFeatures('named_parameters')>>
function original(named int $b, inout int $a): void {
  $a += $b;
}
