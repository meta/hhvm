<?hh

// Erased type arguments make these factory pointers ===. Equality refinement
// intersects their return types, so one fresh box is treated as both
// Break20Box<int> and Break20Box<arraykey>, without memoization.
final class Break20 {
  public static function run(): int {
    $int_factory = self::make<int>;
    $arraykey_factory = self::make<arraykey>;
    if ($int_factory === $arraykey_factory) {
      $box = $int_factory();
      self::write($box);
      return $box->get();
    }
    return 0;
  }

  private static function make<T>(): Break20Box<T> {
    return new Break20Box<T>();
  }

  private static function write(Break20Box<arraykey> $box): void {
    $box->set('foo');
  }
}

final class Break20Box<T> {
  private ?T $value = null;

  public function set(T $value): void {
    $this->value = $value;
  }

  public function get(): T {
    return $this->value as nonnull;
  }
}
