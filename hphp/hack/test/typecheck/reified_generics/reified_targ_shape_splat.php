<?hh
<<file: __EnableUnstableFeatures('shape_splat_concrete', 'shape_splat_type_parameters')>>

class C<reify T> {}
function f<reify T>(): void {}

type TNothing = nothing;
type TSplat = shape('c' => int, ...shape(?'c' => int));
newtype TSplatParam<TShape as shape(...)> = shape(...TShape, 'd' => int);
type TNested = shape('s' => TSplat);

function bad_param(C<TSplat> $_): void {}
function bad_nested(C<TNested> $_): void {}

function bad_targs<reify T as shape(...)>(): void {
  f<shape(...shape('a' => int), ...shape(?'a' => string))>();
  f<shape(...shape(...), 'a' => int)>();
  f<shape(...T, 'c' => int)>();
  new C<TSplatParam<shape('d' => int)>>();
  f<shape('a' => int, ...shape(?'a' => noreturn))>();
}

function good(C<shape('a' => int, ...shape('b' => string))> $_): void {
  f<shape(...shape(?'a' => int), ...shape('b' => int))>();
  f<shape('a' => int, ...shape(?'a' => nothing))>();
  f<shape('a' => int, ...nothing)>();
  f<shape('a' => int, ...shape(?'a' => TNothing))>();
  new C<TSplatParam<shape('a' => int)>>();
  f<shape('c' => int, ...)>();
}
