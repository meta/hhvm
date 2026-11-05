<?hh

// Enforcing a parameter or return hint that applies a type alias or generic
// class to a reified generic must not retain memory per call.

type Bound<T> = shape('value' => T, 'is_inclusive' => bool);
type Box<T> = vec<T>;

const int ITERATIONS = 10000;
// A leak of a few hundred bytes per call retains several MiB here.
const int MAX_GROWTH_BYTES = 256 * 1024;

final class Reified<reify T as arraykey> {
  public function takesShape(?Bound<T> $bound): void {}
  public function takesBox(Box<T> $values): void {}
  public function returnsShape(T $value): Bound<T> {
    return shape('value' => $value, 'is_inclusive' => true);
  }
  public function returnsSelf(): Reified<T> {
    return $this;
  }
  public function takesInlineShape(
    shape('value' => T, 'is_inclusive' => bool) $bound,
  ): void {}
  public function takesT(T $value): void {}
}

final class Erased<T as arraykey> {
  public function takesShape(?Bound<T> $bound): void {}
}

function reified_function<reify T as arraykey>(?Bound<T> $bound): void {}

function check(string $label, (function(): void) $call): void {
  // Settle lazy allocations before measuring request-heap usage.
  for ($i = 0; $i < 100; $i++) {
    $call();
  }
  gc_collect_cycles();
  $before = memory_get_usage();
  for ($i = 0; $i < ITERATIONS; $i++) {
    $call();
  }
  gc_collect_cycles();
  $growth = memory_get_usage() - $before;
  echo $label.': '.($growth < MAX_GROWTH_BYTES ? "no leak\n" : "leak\n");
}

<<__EntryPoint>>
function main(): void {
  $reified = new Reified<int>();
  $erased = new Erased<int>();
  $bound = shape('value' => 5, 'is_inclusive' => true);

  check('?Bound<T> param', () ==> $reified->takesShape($bound));
  check('Box<T> param', () ==> $reified->takesBox(vec[5]));
  check('Bound<T> return', () ==> { $reified->returnsShape(5); });
  check('Reified<T> return', () ==> { $reified->returnsSelf(); });
  check('reified function ?Bound<T> param', () ==> reified_function<int>($bound));

  check('?Bound<T> param, null passed', () ==> $reified->takesShape(null));
  check('inline shape param', () ==> $reified->takesInlineShape($bound));
  check('T param', () ==> $reified->takesT(5));
  check('erased ?Bound<T> param', () ==> $erased->takesShape($bound));
}
