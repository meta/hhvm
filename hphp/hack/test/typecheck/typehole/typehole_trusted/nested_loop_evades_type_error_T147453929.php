<?hh

<<__EntryPoint>>
function hhvm_goes_boom(): void {
  $oops = vec[42];

  while (true) {
    while (true) {
      $oops = $oops[0];
    }
  }
}
