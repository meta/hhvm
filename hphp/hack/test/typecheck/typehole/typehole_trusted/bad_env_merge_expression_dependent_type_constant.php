<?hh

class Box<<<__Enforceable>> reify T> {}

abstract class Base {
  <<__Enforceable, __Reifiable>>
  abstract const type T;

  abstract public function get(): Box<this::T>;

  abstract public function put(Box<this::T> $b): void;
}

final class StringBase extends Base {
  const type T = string;

  public function get(): Box<string> {
    return new Box<string>();
  }

  public function put(Box<string> $b): void {}
}

function refine_in_one_branch(Base $x): void {
  $b = $x->get();
  if ($b is Box<int>) {
  }
  $x->put(new Box<int>());
}

<<__EntryPoint>>
function main(): void {
  refine_in_one_branch(new StringBase());
}
