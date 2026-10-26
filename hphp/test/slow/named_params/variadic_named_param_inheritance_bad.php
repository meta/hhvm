<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

interface VariadicNamedContract {
  public function f(named mixed...): void;
}

final class MissingVariadicNamedParameter implements VariadicNamedContract {
  public function f(optional named mixed $known): void {}
}

<<__EntryPoint>>
function main(): void {
  new MissingVariadicNamedParameter();
}
