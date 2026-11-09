<?hh

class Box<<<__Enforceable>> reify T> {}

interface I {}

interface J<T as arraykey> extends I {
  public function get(): T;
}

abstract class Base {
  <<__Enforceable, __Reifiable>>
  abstract const type T;

  public static abstract function take(Box<this::T> $b): void;

  <<__NeedsConcrete>>
  public static function in_branch(Box<this::T> $b, Box<int> $i): void {
    if ($b is Box<int>) {
      static::take($i);
    }
  }

  <<__NeedsConcrete>>
  public static function other_branch_exits(
    Box<this::T> $b,
    Box<int> $i,
  ): void {
    if ($b is Box<int>) {
    } else {
      return;
    }
    static::take($i);
  }

  <<__NeedsConcrete>>
  public static function both_branches(
    Box<this::T> $b,
    Box<int> $i,
    bool $c,
  ): void {
    if ($c) {
      invariant($b is Box<int>, 'refined in then');
    } else {
      invariant($b is Box<int>, 'refined in else');
    }
    static::take($i);
  }
}

function fresh_generic_flows_out(I $a): ?arraykey {
  if ($a is J<_>) {
    $y = $a;
  } else {
    $y = null;
  }
  return $y?->get();
}
