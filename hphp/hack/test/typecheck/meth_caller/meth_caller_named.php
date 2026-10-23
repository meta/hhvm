<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

class MethCallerNamed {
  public function required(named int $x): void {}

  public function mixed(int $x, named int $y): void {}

  public function optional(named int $x = 0): void {}

  public function multiple(named int $x, named int $y): void {}

  // TODO(T294006289): restore when inout and named are allowed in the same definition
  // public function inout_and_named(inout int $x, named int $y): void {}

  public function positional(int $x, int ...$rest): void {}
}

function test_meth_caller_named(): void {
  HH\meth_caller(MethCallerNamed::class, 'required');
  HH\meth_caller(MethCallerNamed::class, 'mixed');
  HH\meth_caller(MethCallerNamed::class, 'optional');
  HH\meth_caller(MethCallerNamed::class, 'multiple');
  // TODO(T294006289): restore when inout and named are allowed in the same definition
  // HH\meth_caller(MethCallerNamed::class, 'inout_and_named');
  $f = HH\meth_caller(MethCallerNamed::class, 'positional');
  $f(new MethCallerNamed(), 1, 2, 3);
}
