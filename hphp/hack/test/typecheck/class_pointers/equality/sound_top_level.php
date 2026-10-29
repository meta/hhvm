<?hh

class P {
  public static function pm(): void {}
}
class C1 extends P {
  public static function m(): void {}
}

function expect_c1(class<C1> $_): void {}

enum E: string {
  A = 'C1';
}
abstract class WithTypeConsts {
  abstract const type TS as string;
}
class Holder {
  public string $s = '';
}

// The local can hold a string, so after the comparison it is at most a name:
// every expect_c1 / ::m() below should be an error.

function sound_string_literal(string $s): void {
  $c = C1::class;
  if ($s === $c) {
    expect_c1($s);
  }
}

function sound_string_var(string $s, class<C1> $c): void {
  if ($s === $c) {
    expect_c1($s);
  }
}

function sound_classname_guard(classname<C1> $cn, class<C1> $c): void {
  if ($cn !== $c) {
    return;
  }
  $cn::m();
}

function sound_classname_inferred(P $p): void {
  $cn = HH\class_to_classname(get_class($p));
  $c = C1::class;
  if ($cn === $c) {
    expect_c1($cn);
  }
}

function sound_arraykey(arraykey $k, class<C1> $c): void {
  if ($k === $c) {
    expect_c1($k);
  }
}

function sound_nullable_string(?string $s, class<C1> $c): void {
  if ($s === $c) {
    expect_c1($s);
  }
}

function sound_mixed(mixed $m, class<C1> $c): void {
  if ($m === $c) {
    expect_c1($m);
  }
}

function sound_nullable_operand(string $s, ?class<C1> $c): void {
  if ($s === $c) {
    expect_c1($s);
  }
}

function sound_union_operand(string $s, (class<C1> | int) $c): void {
  if ($s === $c) {
    expect_c1($s);
  }
}

function sound_reversed(string $s, class<C1> $c): void {
  if ($c === $s) {
    expect_c1($s);
  }
}

function sound_loose(string $s, class<C1> $c): void {
  if ($s == $c) {
    expect_c1($s); // error: `==` does not refine
  }
}

function sound_conjunction(string $s, ?class<C1> $c): void {
  if ($c !== null && $s === $c) {
    expect_c1($s);
  }
}

function sound_invariant(string $s, class<C1> $c): void {
  invariant($s === $c, 'same');
  expect_c1($s);
}

function sound_property(Holder $h, class<C1> $c): void {
  if ($h->s === $c) {
    expect_c1($h->s);
  }
}

function sound_generic_local<T as arraykey>(T $t, class<C1> $c): void {
  if ($t === $c) {
    expect_c1($t);
  }
}

function sound_enum_local(E $e, class<C1> $c): void {
  if ($e === $c) {
    expect_c1($e);
  }
}

abstract class SoundTypeConstLocal extends WithTypeConsts {
  public function f(this::TS $s, class<C1> $c): void {
    if ($s === $c) {
      expect_c1($s);
    }
  }
}

function sound_both_locals(classname<C1> $cn, class<C1> $c): void {
  if ($cn === $c) {
    expect_c1($cn);
  }
}
