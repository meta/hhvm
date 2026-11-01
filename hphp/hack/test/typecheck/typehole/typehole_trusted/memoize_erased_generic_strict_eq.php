<?hh

// Memoize ignores erased generics: getter<int>() and getter<arraykey>() return
// the same Vector, and `===` then refines $vi to both types.
class Break1 {
  public static function run(): int {
    $vi = self::getter<int>();
    $va = self::getter<arraykey>();
    if ($vi === $va) {
      self::vecarr($vi);
    }
    return $vi[0];
  }

  public static function vecarr(Vector<arraykey> $va): void {
    $va[] = 'lol';
  }

  <<__Memoize(#MakeICInaccessible)>>
  public static function getter<T>(): Vector<T> {
    return Vector {};
  }
}
