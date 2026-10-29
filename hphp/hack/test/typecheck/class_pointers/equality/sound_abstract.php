<?hh
<<file:__EnableUnstableFeatures('case_types')>>

class P {
  public static function pm(): void {}
}
class C1 extends P {
  public static function m(): void {}
}

function expect_c1(class<C1> $_): void {}

case type CT = class<C1> | int;
case type GCT<T> = class<T> | int;
enum class EC: mixed {
  class<C1> A = C1::class;
}
abstract class WithTypeConsts {
  abstract const type TC as class<C1>;
}
class Holder {
  public class<C1> $c = C1::class;
}
function apply_to<T>(T $x, (function(T): void) $f): void {
  $f($x);
}

// The local can hold a string, so after the comparison it is at most a name:
// every expect_c1 / ::m() below should be an error.

function sound_generic_bound<T as class<C1>>(string $s, T $t): void {
  if ($s === $t) {
    expect_c1($s);
  }
}

function sound_generic_nullable_bound<T as ?class<C1>>(string $s, T $t): void {
  if ($s === $t) {
    expect_c1($s);
  }
}

function sound_generic_union_bound<T as (class<C1> | int)>(
  string $s,
  T $t,
): void {
  if ($s === $t) {
    expect_c1($s);
  }
}

function sound_generic_vec_bound<T as vec<class<C1>>>(vec<string> $v, T $t): void {
  if ($v === $t) {
    expect_c1($v[0]);
  }
}

function sound_nested_generic_bound<T as class<C1>, TV as vec<T>>(
  vec<string> $v,
  TV $t,
): void {
  if ($v === $t) {
    expect_c1($v[0]);
  }
}

function sound_vec_of_generic<T as class<C1>>(vec<string> $v, vec<T> $x): void {
  if ($v === $x) {
    expect_c1($v[0]);
  }
}

function sound_intersection_operand<T>(string $s, (T & class<C1>) $x): void {
  if ($s === $x) {
    expect_c1($s);
  }
}

function sound_case_type(string $s, CT $ct): void {
  if ($s === $ct) {
    expect_c1($s);
  }
}

function sound_generic_case_type(string $s, GCT<C1> $ct): void {
  if ($s === $ct) {
    expect_c1($s);
  }
}

abstract class SoundTypeConstOperand extends WithTypeConsts {
  public function f(string $s, this::TC $x): void {
    if ($s === $x) {
      expect_c1($s);
    }
  }
}

function sound_enum_class_constant(string $s): void {
  if ($s === EC::A) {
    expect_c1($s);
  }
}

function sound_nullsafe_operand(string $s, ?Holder $h): void {
  if ($s === $h?->c) {
    expect_c1($s);
  }
}

function sound_lambda_param(string $s): void {
  apply_to(C1::class, $c ==> {
    if ($s === $c) {
      expect_c1($s);
    }
  });
}

function sound_unbounded_generic<T>(mixed $m, T $t): T {
  if ($m === $t) {
    hh_show($m);
    return $m;
  }
  return $t;
}
