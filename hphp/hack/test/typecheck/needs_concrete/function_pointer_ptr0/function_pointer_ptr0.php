<?hh

abstract class A {
  abstract public static function hook(): int;

  <<__NeedsConcrete>>
  public static function m(): int {
    return static::hook();
  }
}

function f(): void {
  A::m();
  $_ = A::m<>;
}
