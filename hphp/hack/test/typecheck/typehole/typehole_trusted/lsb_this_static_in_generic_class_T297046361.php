<?hh

class LsbBox<T> {
  <<__LSB>> private static ?this $slot = null;

  public function __construct(private T $v) {}

  public function get(): T {
    return $this->v;
  }

  public function publish(): void {
    static::$slot = $this;
  }

  public function readPublished(): T {
    return (static::$slot as nonnull)->get();
  }
}

function expect_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  (new LsbBox('not an int'))->publish();
  expect_int((new LsbBox(42))->readPublished());
}
