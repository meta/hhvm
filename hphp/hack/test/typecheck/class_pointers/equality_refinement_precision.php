<?hh

class P {
  public static function pm(): void {}
}
class C1 extends P {
  public static function m(): void {}
}

function expect_c1(class<C1> $_): void {}

function class_local(class<P> $c): void {
  if ($c === C1::class) {
    expect_c1($c);
    $c::m();
  }
}

function nullable_class_local(?class<P> $c): void {
  if ($c === C1::class) {
    expect_c1($c);
  }
}

function generic_class_local<T as P>(class<T> $c): void {
  if ($c === C1::class) {
    expect_c1($c);
  }
}

abstract class Base {
  public static function test(): void {
    $cls = static::class;
    if ($cls === Derived::class) {
      $cls::dm();
    }
  }
}
final class Derived extends Base {
  public static function dm(): void {}
}

function vec_local(vec<class<P>> $v): void {
  if ($v === vec[C1::class]) {
    foreach ($v as $c) {
      expect_c1($c);
    }
  }
}

function dict_local(dict<string, class<P>> $d): void {
  if ($d === dict['a' => C1::class]) {
    expect_c1($d['a']);
  }
}

function tuple_local((class<P>, int) $t): void {
  if ($t === tuple(C1::class, 1)) {
    expect_c1($t[0]);
  }
}

function shape_local(shape('c' => class<P>) $s): void {
  if ($s === shape('c' => C1::class)) {
    expect_c1($s['c']);
  }
}

function imm_vector_local(ImmVector<class<P>> $v): void {
  if ($v === ImmVector {C1::class}) {
    foreach ($v as $c) {
      expect_c1($c);
    }
  }
}

function keyed_container_local(KeyedContainer<int, class<P>> $kc): void {
  if ($kc === vec[C1::class]) {
    foreach ($kc as $c) {
      expect_c1($c);
    }
  }
}

function traversable_local(Traversable<class<P>> $t): void {
  if ($t === vec[C1::class]) {
    foreach ($t as $c) {
      expect_c1($c);
    }
  }
}
