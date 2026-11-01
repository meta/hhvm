<?hh

// Private members skip variance checks but are reachable on any instance, so a
// covariant Box<int> passed as Box<arraykey> gets a string written into it.
final class Box<+T> {
  public function __construct(private T $t) {}
  public function get(): T {
    return $this->t;
  }
  public static function overwrite(Box<arraykey> $box, arraykey $in): void {
    $box->t = $in;
  }
}

function break_it(): int {
  $int_box = new Box<int>(1);
  Box::overwrite($int_box, 'foo');
  return $int_box->get();
}
