<?hh

class Box<<<__Enforceable>> reify T> {}

final class C<<<__Enforceable>> reify T> {
  public function take(T $_): void {}

  // The locals are unioned under the facts of the else branch, where T = int,
  // so int | T simplifies to T.
  public function refined_in_else(Box<T> $b, bool $c, T $t): void {
    if ($c) {
      $v = 1;
    } else {
      invariant($b is Box<int>, 'refined in else');
      $v = $t;
    }
    $this->take($v);
  }
}

function function_tparam<<<__Enforceable>> reify T>(
  Box<T> $b,
  bool $c,
  T $t,
  (function(T): void) $f,
): void {
  if ($c) {
    $v = 1;
  } else {
    invariant($b is Box<int>, 'refined in else');
    $v = $t;
  }
  $f($v);
}

<<__EntryPoint>>
function main(): void {
  (new C<string>())->refined_in_else(new Box<string>(), true, '');
}
