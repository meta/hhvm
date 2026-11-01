<?hh

// An override may add a reified type parameter, so a call through the parent
// type supplies no generics.
class D {
  public function bar(): void {}
}

class C extends D {
  public function bar<reify T>(): void {}
}

function hhvm_goes_boom(D $d): void {
  $d->bar(); // No error
}

<<__EntryPoint>>
function main(): void {
  // new C()->bar(); // Error
  hhvm_goes_boom(new C());
}
