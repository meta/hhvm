<?hh

// A lone variadic against a lone splat is checked by packaging the variadic
// side into a tuple. Scalar-element variadics satisfy splats of variadic
// tuples, but tuple-element variadics do not satisfy splats of fixed tuples:
// that would be unsound, as the implementation would receive scalars where it
// declares tuples.

function takes_splat_str((function(...(string...)): void) $f): void {}

function takes_splat_arr((function(int, ...(arraykey)): void) $f): void {}

function str_impl(string ...$values): void {}

function test_ok(): void {
  takes_splat_str(str_impl<>);
  takes_splat_str((string ...$values) ==> {});
  // Scalar or mixed elements accept the unpacked scalars.
  takes_splat_arr(($_vc, arraykey ...$keys) ==> {});
  takes_splat_arr(($_vc, mixed ...$keys) ==> {});
}

// Tuple-element variadic against a splat of a fixed tuple: unsound, error.
interface SplatArr<T as (arraykey)> {
  public function f(...T $values): void;
}

final class ImplArr implements SplatArr<(arraykey)> {
  public function f((arraykey) ...$values): void {}
}

function arr_impl(int $vc, (arraykey) ...$keys): void {}

function takes_variadic_arr((function((arraykey)...): void) $f): void {}

function test_bad(): void {
  takes_splat_arr(arr_impl<>);
  takes_splat_arr(($vc, (arraykey) ...$keys) ==> {});
  // Unannotated elements inherit the whole splat tuple, which is then
  // correctly rejected as a tuple-element variadic.
  takes_splat_arr(($_vc, ...$keys) ==> {});
  takes_splat_str((int ...$xs) ==> {});
}

// Mirror direction: a splat of a fixed tuple does not satisfy a
// tuple-element variadic. (The arity error is pre-existing: a splat does not
// count as a variadic argument. The subtyping error is the soundness fix.)
function test_mirror_bad((function(...(arraykey)): void) $g): void {
  takes_variadic_arr($g);
}
