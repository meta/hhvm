<?hh

class P {
  public static function pm(): void {}
}
class C1 extends P {
  public static function m(): void {}
}

function expect_c1(class<C1> $_): void {}
function expect_vc1(vec<class<C1>> $_): void {}

// The local is a class pointer, so after the comparison it is a class<C1>:
// none of the expect_c1 / ::m() below should be an error.
// BAD: the filter rejects strict refinements here, so these cases lose precision.

function precise_class(class<P> $c): void {
  if ($c === C1::class) {
    expect_c1($c);
    $c::m();
  }
}

function precise_nullable_class(?class<P> $c): void {
  if ($c === C1::class) {
    expect_c1($c);
  }
}

function precise_generic_class<T as P>(class<T> $c): void {
  if ($c === C1::class) {
    expect_c1($c);
  }
}

abstract class PreciseStatic {
  public static function test(): void {
    $cls = static::class;
    if ($cls === PreciseDerived::class) {
      $cls::dm();
    }
  }
}

final class PreciseDerived extends PreciseStatic {
  public static function dm(): void {}
}

function precise_class_vs_classname(class<P> $c, classname<C1> $cn): void {
  if ($c === $cn) {
    expect_c1($c); // BAD: false positive, the local is a class pointer
  }
}

function precise_class_vs_class_or_classname(
  class<P> $c,
  HH\class_or_classname<C1> $x,
): void {
  if ($c === $x) {
    expect_c1($c); // BAD: false positive, the local is a class pointer
  }
}

function precise_vec(vec<class<P>> $v): void {
  if ($v === vec[C1::class]) {
    expect_vc1($v);
    foreach ($v as $c) {
      expect_c1($c);
    }
  }
}

function precise_vec_vs_classname_vec(
  vec<class<P>> $v,
  vec<classname<C1>> $x,
): void {
  if ($v === $x) {
    foreach ($v as $c) {
      expect_c1($c); // BAD: false positive, the local is a class pointer
    }
  }
}

function precise_dict(dict<string, class<P>> $d): void {
  if ($d === dict['k' => C1::class]) {
    expect_c1($d['k']);
  }
}

function precise_tuple((class<P>, int) $t): void {
  if ($t === tuple(C1::class, 1)) {
    expect_c1($t[0]);
  }
}

function precise_tuple_mixed_positions((class<P>, string) $t): void {
  if ($t === tuple(C1::class, nameof C1)) {
    expect_c1($t[0]);
  }
}

function precise_shape(shape('c' => class<P>) $s): void {
  if ($s === shape('c' => C1::class)) {
    expect_c1($s['c']);
  }
}

function precise_shape_mixed_fields(
  shape('c' => class<P>, 's' => string) $s,
): void {
  if ($s === shape('c' => C1::class, 's' => nameof C1)) {
    expect_c1($s['c']);
  }
}

function precise_nested_vec(vec<vec<class<P>>> $v): void {
  if ($v === vec[vec[C1::class]]) {
    expect_c1($v[0][0]);
  }
}

function precise_imm_vector(ImmVector<class<P>> $v): void {
  if ($v === ImmVector {C1::class}) {
    foreach ($v as $c) {
      expect_c1($c);
    }
  }
}

function precise_keyed_container(KeyedContainer<int, class<P>> $kc): void {
  if ($kc === vec[C1::class]) {
    foreach ($kc as $c) {
      expect_c1($c);
    }
  }
}

function precise_traversable(Traversable<class<P>> $t): void {
  if ($t === vec[C1::class]) {
    foreach ($t as $c) {
      expect_c1($c);
    }
  }
}
