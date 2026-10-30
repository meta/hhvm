<?hh

class C {
  public static ?int $x = 1;
  public static ?int $y = 1;

  // C::$x and self::$x are the same property.
  public static function class_name_write(): int {
    if (self::$x is nonnull) {
      C::$x = null;
      return self::$x;
    }
    return 0;
  }

  public static function self_write(): int {
    if (static::$x is nonnull) {
      self::$x = null;
      return static::$x;
    }
    return 0;
  }

  public static function refine_unchanged(): int {
    if (self::$x is nonnull) {
      return self::$x;
    }
    return 0;
  }

  // The assigned spelling keeps the assigned type.
  public static function refine_by_assignment(): int {
    C::$x = 1;
    return C::$x;
  }

  // A different property name keeps its refinement.
  public static function other_property(): int {
    if (self::$y is nonnull) {
      C::$x = null;
      return self::$y;
    }
    return 0;
  }
}
