<?hh

class B {
  public static function foo(): void {}
}

interface C {
  require extends B;
}

<<__EntryPoint>>
function bar(): void {
  B::foo();
  $c = C::class;
  $c::foo();
}
