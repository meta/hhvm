<?hh
// @no-wrapper
<<file: __EnableUnstableFeatures('named_parameters')>>
class C {
  public static function original(inout int $a, named int $b = 1): void {
    $a += $b;
  }
}
