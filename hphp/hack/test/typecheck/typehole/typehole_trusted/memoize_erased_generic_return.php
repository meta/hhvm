<?hh

// Memoize ignores erased generics: getter<int>() and getter<arraykey>() share
// one Vector, so the string written through $va is read back as int.
final class Break2 {
  public static function run(): int {
    $vi = self::getter<int>();
    $va = self::getter<arraykey>();
    $va[] = 'foo';
    return $vi[0];
  }

  <<__Memoize(#MakeICInaccessible)>>
  public static function getter<T>(): Vector<T> {
    return Vector {};
  }
}
