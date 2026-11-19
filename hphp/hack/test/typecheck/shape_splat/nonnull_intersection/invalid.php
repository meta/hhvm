<?hh
<<file:__EnableUnstableFeatures(
  'shape_splat_concrete',
  'shape_splat_expression',
  'union_intersection_type_hints',
)>>

function bare(nonnull $x): void {
  $s = shape(...$x, 'f' => 1);
}

function repeated((nonnull & nonnull) $x): void {
  $s = shape(...$x, 'f' => 1);
}

function incompatible((nonnull & int) $x): void {
  $s = shape(...$x, 'f' => 1);
}

function mixed_operand(mixed $x): void {
  $s = shape(...$x, 'f' => 1);
}

function incompatible_hint(shape(...(nonnull & int), 'f' => int) $x): void {}
