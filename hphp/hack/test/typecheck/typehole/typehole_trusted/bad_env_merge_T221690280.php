<?hh

class MyReifiedGenericClass<<<__Enforceable>> reify T> {}

abstract class Foo {
  <<__Enforceable, __Reifiable>>
  abstract const type TInner;
  const type TOuter = MyReifiedGenericClass<this::TInner>;

  public static abstract function takeOuter(this::TOuter $outer): void;

  <<__NeedsConcrete>>
  public static function test(
    this::TOuter $outer,
    MyReifiedGenericClass<int> $i,
  ): void {
    if ($outer is MyReifiedGenericClass<int>) {
      static::takeOuter($i); // should not error, does not error
    } else {
      static::takeOuter($i); // should error, does error
    }
    static::takeOuter($i); // should error, does NOT error (BUG)
  }
}
