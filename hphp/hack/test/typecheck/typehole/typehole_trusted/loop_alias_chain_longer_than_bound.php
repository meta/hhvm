<?hh

// A loop is typed for as many iterations as its alias chain is deep, but a
// value takes one more iteration to reach the start of the loop. The string
// assigned to $x reaches $z only on the fourth iteration.
function takes_nullable_int(?int $_): void {}

<<__EntryPoint>>
function main(): void {
  $x = null;
  $y = null;
  $z = null;
  foreach (vec[1, 2, 3, 4] as $_) {
    takes_nullable_int($z);
    $z = $y;
    $y = $x;
    $x = 'a';
  }
}
