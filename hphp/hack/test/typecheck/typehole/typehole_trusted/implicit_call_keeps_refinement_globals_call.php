<?hh

final class C {
  private static ?int $x = 1;
  public ?int $y = 1;

  // A [globals] call can write static properties.
  public static function globals_call(): int {
    if (self::$x is nonnull) {
      self::clear();
      return self::$x;
    }
    return 0;
  }

  private static function clear()[globals]: void {
    self::$x = null;
  }

  // Calls that can't touch globals or properties keep refinements.
  public static function pure_call(): int {
    if (self::$x is nonnull) {
      self::pure();
      return self::$x;
    }
    return 0;
  }

  private static function pure()[]: void {}

  public static function read_globals_call(): int {
    if (self::$x is nonnull) {
      self::read();
      return self::$x;
    }
    return 0;
  }

  private static function read()[read_globals]: void {}
}
