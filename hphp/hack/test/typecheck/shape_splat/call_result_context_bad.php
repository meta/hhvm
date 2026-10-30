<?hh
<<file:__EnableUnstableFeatures(
  'shape_splat_type_parameters',
  'shape_splat_expression',
  'like_type_hints',
)>>

function string_value(): string {
  return 'text';
}

function wrong_payload<TRest as supportdyn<shape(...)>>(
  TRest $s,
): ~shape(...TRest, 'payload' => int) {
  return shape(...$s, 'payload' => string_value());
}

function wrong_payload_local<TRest as supportdyn<shape(...)>>(
  TRest $s,
): ~shape(...TRest, 'payload' => int) {
  $payload = string_value();
  return shape(...$s, 'payload' => $payload);
}

function wrong_rest<TRest as supportdyn<shape(...)>>(
  TRest $_,
): ~shape(...TRest, 'payload' => int) {
  return shape('payload' => 1);
}

function wrong_row_identity<
  TLeft as supportdyn<shape(...)>,
  TRight as supportdyn<shape(...)>,
>(
  TLeft $left,
  TRight $_right,
): ~shape(...TRight, 'payload' => int) {
  return shape(...$left, 'payload' => 1);
}

function wrong_spread_order<TRest as supportdyn<shape(...)>>(
  TRest $s,
  (function(): int) $f,
): ~shape(...TRest, 'payload' => int) {
  return shape('payload' => $f(), ...$s);
}

<<__NoAutoLikes>>
function plain_shape_expectation<
  TRest as supportdyn<shape(...)>,
  TOut as supportdyn<mixed>,
>(
  TRest $s,
  (function(): ~TOut) $f,
): shape(...TRest, 'payload' => TOut) {
  return shape(...$s, 'payload' => $f());
}

<<__NoAutoLikes>>
function bad_like_return<T as supportdyn<mixed>>(
  (function(): ~T) $f,
): T {
  return $f();
}

function bad_like_argument<T as supportdyn<mixed>>(
  (function(): ~T) $f,
  (function(T): void) $use,
): void {
  $use($f());
}
