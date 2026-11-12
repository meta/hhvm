<?hh

class Box<<<__Enforceable>> reify T> {}

abstract class Base {
  <<__Enforceable, __Reifiable>>
  abstract const type T;

  abstract public function get(): Box<this::T>;

  abstract public function make(): this::T;

  abstract public function take(this::T $t): void;
}

final class StringBase extends Base {
  const type T = string;

  public function get(): Box<string> {
    return new Box<string>();
  }

  public function make(): string {
    return '';
  }

  public function take(string $t): void {}
}

// The locals are unioned under the facts of the else branch, where T = int,
// so int | <expr>::T simplifies to <expr>::T.
function refined_in_else(Base $x, bool $c): void {
  if ($c) {
    $v = 1;
  } else {
    $b = $x->get();
    invariant($b is Box<int>, 'refined in else');
    $v = $x->make();
  }
  $x->take($v);
}

function refined_in_loop(Base $x, bool $c): void {
  $v = 1;
  while ($c) {
    $b = $x->get();
    invariant($b is Box<int>, 'refined in the loop');
    $v = $x->make();
    $c = false;
  }
  $x->take($v);
}

<<__EntryPoint>>
function main(): void {
  refined_in_else(new StringBase(), true);
}
