<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

function soft_reified<<<__Soft>> reify T>(named mixed...): void {
  var_dump('soft reified');
}

function hard_reified<reify T>(named mixed...): void {
  var_dump('unreachable');
}

<<__EntryPoint>>
function main(): void {
  \hphp_invoke_callable_named_args(
    soft_reified<>,
    vec[],
    dict['junk' => 1],
  );

  try {
    \hphp_invoke_callable_named_args(
      hard_reified<>,
      vec[],
      dict['junk' => 1],
    );
  } catch (Exception $e) {
    var_dump($e->getMessage());
  }
}
