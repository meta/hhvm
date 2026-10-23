<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

class UnsupportedNamedCaller {
  // TODO(T294006289): restore when inout and named are allowed in the same definition
  // public function inout_and_named(inout int $x, named int $y): void {}

  public function readonly_return(named int $x): readonly UnsupportedNamedCaller {
    return new UnsupportedNamedCaller();
  }
}

function test_unsupported_named_caller(): void {
  // TODO(T294006289): restore when inout and named are allowed in the same definition
  // HH\meth_caller(UnsupportedNamedCaller::class, 'inout_and_named'); // Error
  HH\meth_caller(UnsupportedNamedCaller::class, 'readonly_return'); // Error
}
