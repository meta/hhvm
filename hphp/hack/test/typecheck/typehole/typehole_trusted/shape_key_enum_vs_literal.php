<?hh

// Shape subtyping compares keys by name: Break18Key::K is 'k' at runtime, so a
// string field is accepted as the optional int field 'k'.
class Break18 {
  public static function run(): int {
    $s = self::widen(shape(Break18Key::K => 'foo'));
    return Shapes::idx($s, 'k') ?? 0;
  }

  private static function widen(
    shape(Break18Key::K => string) $s,
  ): shape(?'k' => int, ...) {
    return $s;
  }
}

enum Break18Key: string {
  K = 'k';
}
