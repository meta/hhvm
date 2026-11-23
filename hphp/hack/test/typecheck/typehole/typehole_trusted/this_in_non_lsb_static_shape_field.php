<?hh

// A non-LSB static is shared by every subclass, but `this` in a shape field
// of its type is never checked.
class P {
  protected static ?shape('a' => this) $s = null;
  public function put(): void { static::$s = shape('a' => $this); }
  public static function get(): ?shape('a' => this) { return static::$s; }
}
final class A extends P {}
final class B extends P { public function bOnly(): int { return 1; } }
<<__EntryPoint>> function main(): void {
  (new A())->put();
  $x = B::get();
  if ($x is nonnull) { var_dump($x['a']->bOnly()); }
}
