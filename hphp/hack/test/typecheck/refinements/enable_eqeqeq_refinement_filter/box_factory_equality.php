<?hh

class Box<T> {
  private ?T $t = null;

  public function get(): T {
    return $this->t as nonnull;
  }

  public function set(T $t): void {
    $this->t = $t;
  }
}

class Factory {
  <<__Memoize>>
  public static function make<T>(): Box<T> {
    return new Box();
  }

  public static function typed(): int {
    $bi = self::make<int>();
    $ba = self::make<arraykey>();
    if ($bi === $ba) {
      $bi->set('str');
    }
    return $bi->get();
  }
}

<<__EntryPoint>>
function main(): void {
  Factory::typed();
}
