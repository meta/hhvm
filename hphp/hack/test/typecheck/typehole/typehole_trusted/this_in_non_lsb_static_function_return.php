<?hh

// A non-LSB static is shared by every subclass, but `this` in a function
// return type in its type is never checked.
class P {
  protected static ?(function(): this) $f = null;
  public function put(): void { static::$f = () ==> $this; }
  public static function get(): ?(function(): this) { return static::$f; }
}
final class A extends P {}
final class B extends P { public function bOnly(): int { return 1; } }
<<__EntryPoint>> function main(): void {
  (new A())->put();
  $f = B::get();
  if ($f is nonnull) { var_dump($f()->bOnly()); }
}
