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

<<__Memoize>>
function getter<T>(): Box<T> {
  return new Box();
}

function bad(Box<int> $bi, Box<arraykey> $ba): void {
  if ($bi === $ba) {
    $bi->set('str');
  }
}

function typed(Box<int> $bi, Box<arraykey> $ba): int {
  bad($bi, $ba);
  return $bi->get();
}

<<__EntryPoint>>
function main(): void {
  $bi = getter<int>();
  $ba = getter<arraykey>();
  typed($bi, $ba);
}
