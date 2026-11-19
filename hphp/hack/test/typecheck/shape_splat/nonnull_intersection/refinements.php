<?hh
<<file:__EnableUnstableFeatures(
  'shape_splat_expression',
  'shape_splat_type_parameters',
  'union_intersection_type_hints',
  'like_type_hints',
)>>

function coalesced(~?shape('a' => int) $alt, int $x): void {
  $s = shape(...($alt ?? shape()), 'f' => 1);
  hh_show($s);
  if ($x > 0) {
    $s['w0'] = true;
  }
  if ($x > 1) {
    $s['w1'] = true;
  }
  if ($x > 2) {
    $s['w2'] = true;
  }
  hh_show($s);
}

function dynamic_first((dynamic & nonnull) $x): void {
  hh_show(shape(...$x, 'f' => 1));
}

function nonnull_first((nonnull & dynamic) $x): void {
  hh_show(shape(...$x, 'f' => 1));
}

function nested((nonnull & (dynamic & nonnull)) $x): void {
  hh_show(shape(...$x, 'f' => 1));
}

function nullable_generic<T as shape(...)>((?T & nonnull) $x): void {
  hh_show(shape(...$x, 'f' => 1));
}

function row_intersection<T1 as shape(...), T2 as shape(...)>(
  (T1 & nonnull & T2) $x,
): void {
  hh_show(shape(...$x, 'f' => 1));
}

function bottom((nothing & nonnull) $x): void {
  hh_expect<nothing>(shape(...$x, 'f' => 1));
}
