<?hh

class Box<<<__Enforceable>> reify T> {}

abstract class Base {
  <<__Enforceable, __Reifiable>>
  abstract const type T;

  public static abstract function take(Box<this::T> $b): void;

  abstract public function get(): Box<this::T>;

  abstract public function put(Box<this::T> $b): void;

  <<__NeedsConcrete>>
  public static function if_without_else(Box<this::T> $b, Box<int> $i): void {
    if ($b is Box<int>) {
    }
    static::take($i);
  }

  <<__NeedsConcrete>>
  public static function ternary(Box<this::T> $b, Box<int> $i): void {
    $_ = $b is Box<int> ? 1 : 2;
    static::take($i);
  }

  <<__NeedsConcrete>>
  public static function in_loop(
    Box<this::T> $b,
    Box<int> $i,
    vec<int> $v,
  ): void {
    foreach ($v as $_) {
      if ($b is Box<int>) {
      }
    }
    static::take($i);
  }

  <<__NeedsConcrete>>
  public static function in_switch(
    Box<this::T> $b,
    Box<int> $i,
    int $k,
  ): void {
    switch ($k) {
      case 1:
        invariant($b is Box<int>, 'refined in one case');
        break;
      default:
        break;
    }
    static::take($i);
  }

  <<__NeedsConcrete>>
  public static function in_try(Box<this::T> $b, Box<int> $i): void {
    try {
      invariant($b is Box<int>, 'refined in try');
    } catch (Exception $_) {
    }
    static::take($i);
  }
}

function expression_dependent(Base $x, Box<int> $i): void {
  $b = $x->get();
  if ($b is Box<int>) {
  }
  $x->put($i);
}
