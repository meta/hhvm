<?hh

final class Holder {
  public ?int $x = 1;
}

// The left operand reads x before clear() sets it to null. Equality refinement
// then restores the stale nonnull type on the property itself.
final class C {
  public static function run(): int {
    $holder = new Holder();
    if ($holder->x === self::clear($holder)) {
      return $holder->x;
    }
    return 0;
  }

  private static function clear(Holder $holder): int {
    $holder->x = null;
    return 1;
  }
}
