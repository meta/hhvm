<?hh

// Hand-rolled memoization via `as this`: every Box::shared() call gets a new
// T, but the runtime only checks the class, so one box serves every T.
final class Box<T> {
  private static mixed $shared = null;
  private ?T $t = null;
  public static function shared(): this {
    $box = self::$shared ?? new self();
    self::$shared = $box;
    return $box as this;
  }
  public function set(T $t): void {
    $this->t = $t;
  }
  public function get(): T {
    return $this->t as nonnull;
  }
}
