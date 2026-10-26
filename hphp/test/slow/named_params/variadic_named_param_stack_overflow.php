<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

function sink(named mixed...): void {}

<<__EntryPoint>>
function main(): void {
  $args = dict[];
  for ($i = 0; $i < 10000; ++$i) {
    $args[(string)$i] = $i;
  }
  \hphp_invoke_callable_named_args(sink<>, vec[], $args);
}
