<?hh
<<file:__EnableUnstableFeatures('shape_splat_expression')>>

class C {}

function controls(
  shape('c' => string) $strings,
  shape('c' => class<C>) $classes,
  shape('c' => int) $ints,
): void {
  if ($strings === shape(...shape('c' => nameof C))) {
  }
  if ($classes === shape(...shape('c' => C::class))) {
  }
  if (shape(...$classes) == $strings) {
  }
  if ($strings === shape(...shape('c' => 'C'))) {
  }
  if ($ints === shape(...shape('c' => 123))) {
  }
  switch ($classes) {
    case shape(...shape('c' => C::class)):
      break;
    default:
      break;
  }
}

function reified<reify T as C>(shape('c' => string) $strings): bool {
  return $strings === shape(...shape('c' => T::class));
}
