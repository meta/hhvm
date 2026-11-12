<?hh

// A loop inside a lambda inside a loop is typed for one iteration only, so the
// int assigned on the first iteration reaches ?string on the second.
function takes_nullable_str(?string $_): void {}

<<__EntryPoint>>
function main(): void {
  foreach (vec[1] as $_) {
    $g = () ==> {
      $prev = null;
      foreach (vec[1, 2] as $i) {
        takes_nullable_str($prev);
        $prev = $i;
      }
    };
    $g();
  }
}
