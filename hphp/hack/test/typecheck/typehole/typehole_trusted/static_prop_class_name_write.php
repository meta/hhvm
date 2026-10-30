<?hh

// Writing the static property as C::$x doesn't invalidate the refinement of
// the same property spelled self::$x.
class C {
  public static ?int $x = 1;

  public static function run(): int {
    if (self::$x is nonnull) {
      C::$x = null;
      return self::$x;
    }
    return 0;
  }
}
