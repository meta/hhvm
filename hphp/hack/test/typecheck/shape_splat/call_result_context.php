<?hh
<<file:__EnableUnstableFeatures(
  'shape_splat_type_parameters',
  'shape_splat_expression',
  'like_type_hints',
)>>

function map_payload<
  TRest as supportdyn<shape(...)>,
  TIn,
  TOut as supportdyn<mixed>,
>(
  shape(...TRest, 'payload' => TIn) $s,
  (function(TIn): ~TOut) $f,
): ~shape(...TRest, 'payload' => TOut) {
  return shape(...$s, 'payload' => $f($s['payload']));
}

function map_payload_local<
  TRest as supportdyn<shape(...)>,
  TIn,
  TOut as supportdyn<mixed>,
>(
  shape(...TRest, 'payload' => TIn) $s,
  (function(TIn): ~TOut) $f,
): ~shape(...TRest, 'payload' => TOut) {
  $payload = $f($s['payload']);
  return shape(...$s, 'payload' => $payload);
}

function identity<T as supportdyn<mixed>>(~T $value): ~T {
  return $value;
}

function map_payload_generic<
  TRest as supportdyn<shape(...)>,
  TIn,
  TOut as supportdyn<mixed>,
>(
  shape(...TRest, 'payload' => TIn) $s,
  (function(TIn): ~TOut) $f,
): ~shape(...TRest, 'payload' => TOut) {
  return shape(...$s, 'payload' => identity($f($s['payload'])));
}

function nullable_expectation<
  TRest as supportdyn<shape(...)>,
  TOut as supportdyn<mixed>,
>(
  TRest $s,
  (function(): ~TOut) $f,
): ~?shape(...TRest, 'payload' => TOut) {
  return shape(...$s, 'payload' => $f());
}

function supportdyn_expectation<
  TRest as supportdyn<shape(...)>,
  TOut as supportdyn<mixed>,
>(
  TRest $s,
  (function(): ~TOut) $f,
): supportdyn<~shape(...TRest, 'payload' => TOut)> {
  return shape(...$s, 'payload' => $f());
}

function optional_payload<
  TRest as supportdyn<shape('id' => int)>,
  TOut as supportdyn<mixed>,
>(
  TRest $s,
  (function(): ~TOut) $f,
): ~shape(...TRest, ?'payload' => TOut) {
  return shape(...$s, 'payload' => $f());
}

function multiple_rows<
  TLeft as supportdyn<shape(...)>,
  TRight as supportdyn<shape(...)>,
  TOut as supportdyn<mixed>,
>(
  shape(...TLeft, 'x' => int) $left,
  TRight $right,
  (function(): ~TOut) $f,
): ~shape(...TLeft, 'x' => int, ...TRight, 'payload' => TOut) {
  return shape(...$left, ...$right, 'payload' => $f());
}

<<__SupportDynamicType>>
class C {
  public function foo(): int {
    return 1;
  }
}

function make_callback<T as supportdyn<mixed>>(T $value): ~T {
  return $value;
}

function contextual_lambda<TRest as supportdyn<shape(...)>>(
  TRest $s,
): ~shape(...TRest, 'payload' => supportdyn<(function(C): int)>) {
  return shape(...$s, 'payload' => make_callback($x ==> {
    $x->foo();
    return 1;
  }));
}

function test_inference(): void {
  $s = shape('id' => 42, 'payload' => 7);
  $f = (int $n) ==> 'user-'.$n;
  hh_expect<~shape('id' => int, 'payload' => string)>(map_payload($s, $f));
  hh_expect<~shape('id' => int, 'payload' => string)>(map_payload_local($s, $f));
  hh_expect<~shape('id' => int, 'payload' => string)>(map_payload_generic($s, $f));
}
