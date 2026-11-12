<?hh

trait TGet<T> {
  abstract public function get(): T;
}

trait TGetString {
  use TGet<string>;
}

abstract class GetsInt {
  use TGet<int>;
}

abstract class GetsString extends GetsInt {
  use TGetString;
}

final class C extends GetsString {
  public function get(): string {
    return 'not an int';
  }
}

function get_int(GetsInt $g): int {
  return $g->get();
}

<<__EntryPoint>>
function main(): void {
  get_int(new C());
}
