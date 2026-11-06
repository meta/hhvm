<?hh

class C {
  public static function answer(): int { return 42; }
}

// Folding the call into its result would drop the conversion notice.
<<__EntryPoint>>
function main(): void {
  $x = "C";
  echo $x::answer()."\n";
}
