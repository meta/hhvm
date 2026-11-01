<?hh

// In the nested loop $foo is typed null at the `is nonnull` check, so the
// branch treats the int 42 as `nothing` and passes it as a string.
function takes_string(string $_): void {}

<<__EntryPoint>>
function main(): void {
  while (true) {
    $foo = null;
    while (true) {
      if ($foo is nonnull) {
        takes_string($foo);
        continue;
      }
      $foo = 42;
    }
  }
}
