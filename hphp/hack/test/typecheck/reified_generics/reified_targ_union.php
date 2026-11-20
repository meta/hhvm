<?hh
<<file: __EnableUnstableFeatures('union_intersection_type_hints', 'case_types')>>

class C<reify T> {}
function f<reify T>(): void {}

type TUnionAlias = shape('c' => (int | string));
newtype TUnionAsConstraint as (int | string) = int;
case type TCase = int | string;
case type TCaseGeneric<T> = vec<T> | int;

function bad_param(C<(int | string)> $_): void {}
function bad_nested(C<vec<(int | string)>> $_): void {}
function bad_alias(C<TUnionAlias> $_): void {}

function bad_targs(): void {
  f<(int | string)>();
  new C<shape('c' => (int | string))>();
  f<TCaseGeneric<(int | string)>>();
}

function good(
  C<TCase> $_,
  C<TCaseGeneric<int>> $_,
  C<TUnionAsConstraint> $_,
  C<?int> $_,
  C<nothing> $_,
): void {
  f<TCase>();
  new C<vec<int>>();
}
