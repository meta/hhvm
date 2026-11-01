<?hh

final class Wrapper<T> {
  private function __construct(private T $value) {}

  // $x is of type `this = Wrapper<T>`, but to the caller,
  // `T` has not been constrained in any meaningful way.
  public static function getWrapperOfNull(): Wrapper<T> {
    $x = new static(null);
    return $x;
  }

  public function get(): T {
    return $this->value;
  }
}

function takes_string(string $_): void {}

<<__EntryPoint>>
function my_main(): void {
  // $wrapper1 has a hover type of Wrapper<nothing>
  $wrapper1 = Wrapper::getWrapperOfNull();
  takes_string($wrapper1->get());
}
