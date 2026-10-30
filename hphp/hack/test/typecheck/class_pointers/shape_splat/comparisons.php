<?hh
<<file:__EnableUnstableFeatures('shape_splat_expression')>>

class C {}

function compared_with_strings(
  shape('c' => string) $rhs,
  shape('nested' => shape('c' => string)) $nested,
  shape('a' => string, 'b' => string) $multiple,
): void {
  if ($rhs === shape('c' => C::class)) {
  }
  if ($rhs === shape(...shape('c' => C::class))) {
  }
  if (shape(...shape('c' => C::class)) == $rhs) {
  }
  if ($rhs != shape(...shape('c' => C::class))) {
  }
  if (shape(...shape('c' => C::class)) !== $rhs) {
  }
  if ($rhs === shape(...shape(...shape('c' => C::class)))) {
  }
  if ($nested === shape('nested' => shape(...shape('c' => C::class)))) {
  }
  if (
    $multiple ===
      shape(...shape('a' => C::class), ...shape('b' => C::class))
  ) {
  }
  switch ($rhs) {
    case shape(...shape('c' => C::class)):
      break;
    default:
      break;
  }
}
