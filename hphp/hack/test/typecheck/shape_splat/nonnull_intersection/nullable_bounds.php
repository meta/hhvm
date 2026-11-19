<?hh
<<file:__EnableUnstableFeatures(
  'shape_splat_concrete',
  'shape_splat_expression',
  'shape_splat_type_parameters',
  'union_intersection_type_hints',
)>>

function coalesced<T as ?shape(...)>(T $x): nonnull {
  return shape(...($x ?? shape()));
}

function refined<T as ?shape(...)>((T & nonnull) $x): (T & nonnull) {
  return shape(...$x);
}

function nullable_hint<T as ?shape(...)>(shape(...(T & nonnull)) $x): nonnull {
  return $x;
}

function with_field<T as ?shape(...)>((T & nonnull) $x): nonnull {
  return shape(...$x, 'f' => 1);
}

function nonnullable_identity<T as shape(...)>(T $x): T {
  return shape(...$x);
}

function invalid_nullable<T as ?shape(...)>(T $x): nonnull {
  return shape(...$x);
}
