<?hh
// RUN: %hackc compile -vHack.Lang.AllowUnstableFeatures=1 %s | FileCheck %s
<<file:__EnableUnstableFeatures(
  'shape_splat_concrete',
  'shape_splat_type_parameters',
  'union_intersection_type_hints',
)>>

namespace ShapeTypeText;

type Base = shape('hidden' => int);
type Other = shape('other' => string);

final class Keys {
  const string K = 'maybe';
  const type Row = Base;
}

// CHECK-LABEL: .function {{.*}} <"HH\\shape('z'=>HH\\bool)" "HH\\darray" > {{.*}}plain(
// CHECK-SAME: <"HH\\shape('z'=>HH\\bool)" "HH\\darray" > $p)
function plain(shape('z' => bool) $p): shape('z' => bool) {
  return $p;
}

// CHECK-LABEL: .function {{.*}} <"HH\\shape(...ShapeTypeText\\Base)" "HH\\darray" > {{.*}}only_splat(
// CHECK-SAME: <"HH\\shape(...ShapeTypeText\\Base)" "HH\\darray" > $p)
function only_splat(shape(...Base) $p): shape(...Base) {
  return $p;
}

// CHECK-LABEL: .function {{.*}} <"HH\\shape('left'=>HH\\int, ...ShapeTypeText\\Base, ?ShapeTypeText\\Keys::K=>?HH\\bool, ...ShapeTypeText\\Other)" "HH\\darray" > {{.*}}ordered(
// CHECK-SAME: <"HH\\shape('left'=>HH\\int, ...ShapeTypeText\\Base, ?ShapeTypeText\\Keys::K=>?HH\\bool, ...ShapeTypeText\\Other)" "HH\\darray" > $p)
function ordered(
  shape('left' => int, ...Base, ?Keys::K => ?bool, ...Other) $p,
): shape('left' => int, ...Base, ?Keys::K => ?bool, ...Other) {
  return $p;
}

// CHECK-LABEL: .function {{.*}} <"HH\\vec<HH\\shape(...HH\\shape(...ShapeTypeText\\Base, 'inner'=>HH\\string), 'outer'=>HH\\bool)>" "HH\\vec" > {{.*}}nested(
// CHECK-SAME: <"HH\\vec<HH\\shape(...HH\\shape(...ShapeTypeText\\Base, 'inner'=>HH\\string), 'outer'=>HH\\bool)>" "HH\\vec" > $p)
function nested(
  vec<shape(...shape(...Base, 'inner' => string), 'outer' => bool)> $p,
): vec<shape(...shape(...Base, 'inner' => string), 'outer' => bool)> {
  return $p;
}

// CHECK-LABEL: .function {{.*}} <"HH\\shape(...T, 'z'=>HH\\bool)" "HH\\darray" > {{.*}}generic(
// CHECK-SAME: <"HH\\shape(...T, 'z'=>HH\\bool)" "HH\\darray" > $p)
function generic<T as shape(...)>(
  shape(...T, 'z' => bool) $p,
): shape(...T, 'z' => bool) {
  return $p;
}

// CHECK-LABEL: .function {{.*}} <"HH\\shape(...ShapeTypeText\\Keys::Row, 'z'=>HH\\bool)" "HH\\darray" > {{.*}}type_access(
// CHECK-SAME: <"HH\\shape(...ShapeTypeText\\Keys::Row, 'z'=>HH\\bool)" "HH\\darray" > $p)
function type_access(
  shape(...Keys::Row, 'z' => bool) $p,
): shape(...Keys::Row, 'z' => bool) {
  return $p;
}

// CHECK-LABEL: .function {{.*}} <"HH\\shape(...(ShapeTypeText\\Base | ShapeTypeText\\Other), 'z'=>HH\\bool)" "HH\\darray" > {{.*}}union_splat(
// CHECK-SAME: <"HH\\shape(...(ShapeTypeText\\Base | ShapeTypeText\\Other), 'z'=>HH\\bool)" "HH\\darray" > $p)
function union_splat(
  shape(...(Base | Other), 'z' => bool) $p,
): shape(...(Base | Other), 'z' => bool) {
  return $p;
}

// CHECK-LABEL: .function {{.*}} <"HH\\shape(...(ShapeTypeText\\Base & ShapeTypeText\\Other), 'z'=>HH\\bool)" "HH\\darray" > {{.*}}intersection_splat(
// CHECK-SAME: <"HH\\shape(...(ShapeTypeText\\Base & ShapeTypeText\\Other), 'z'=>HH\\bool)" "HH\\darray" > $p)
function intersection_splat(
  shape(...(Base & Other), 'z' => bool) $p,
): shape(...(Base & Other), 'z' => bool) {
  return $p;
}

// CHECK-LABEL: .function {{.*}} <"(HH\\shape(...ShapeTypeText\\Base) | HH\\shape(...ShapeTypeText\\Other))" N > {{.*}}outer_union(
// CHECK-SAME: <"(HH\\shape(...ShapeTypeText\\Base) | HH\\shape(...ShapeTypeText\\Other))" N > $p)
function outer_union(
  (shape(...Base) | shape(...Other)) $p,
): (shape(...Base) | shape(...Other)) {
  return $p;
}

// CHECK-LABEL: .function {{.*}} <"?HH\\shape(...ShapeTypeText\\Base, ...)" "HH\\darray" nullable display_nullable> {{.*}}nullable_open(
// CHECK-SAME: <"?HH\\shape(...ShapeTypeText\\Base, ...)" "HH\\darray" nullable display_nullable> $p)
function nullable_open(?shape(...Base, ...) $p): ?shape(...Base, ...) {
  return $p;
}

// CHECK-LABEL: .function {{.*}} <"HH\\shape(...ShapeTypeText\\Base, 'z'=>HH\\bool, (HH\\int | HH\\string)...)" "HH\\darray" > {{.*}}typed_open(
// CHECK-SAME: <"HH\\shape(...ShapeTypeText\\Base, 'z'=>HH\\bool, (HH\\int | HH\\string)...)" "HH\\darray" > $p)
function typed_open(
  shape(...Base, 'z' => bool, (int | string)...) $p,
): shape(...Base, 'z' => bool, (int | string)...) {
  return $p;
}

// CHECK-LABEL: .function {{.*}} <"HH\\shape(...)" "HH\\darray" > {{.*}}open_only(
// CHECK-SAME: <"HH\\shape(...)" "HH\\darray" > $p)
function open_only(shape(...) $p): shape(...) {
  return $p;
}

// CHECK: .property {{.*}} <"HH\\shape(...T, T...)" "HH\\darray" > value =
class Holder<T as shape(...)> {
  public shape(...T, T...) $value;
}
