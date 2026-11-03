<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

type NamedFunction = (function(named int $x): void);

function positional(int $x): void {}
function named_only(named int $x): void {}
function mixed_params(int $x, named string $y): void {}

function inspect<reify T>(T $callable): void {
  var_dump($callable);
  var_dump(HH\ReifiedGenerics\get_type_structure<T>());
}

<<__EntryPoint>>
function main(): void {
  echo "positional\n";
  inspect<(function(int): void)>(positional<>);

  echo "named\n";
  inspect<(function(named int $x): void)>(named_only<>);

  echo "mixed\n";
  inspect<(function(int, named string $y): void)>(mixed_params<>);

  echo "type alias\n";
  var_dump(type_structure(NamedFunction::class));
}
