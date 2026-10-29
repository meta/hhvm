<?hh

class P {
  public static function pm(): void {}
}
class C1 extends P {
  public static function m(): void {}
}

function expect_c1(class<C1> $_): void {}

// The local can hold a string, so after the comparison it is at most a name:
// every expect_c1 / ::m() below should be an error.

function sound_vec_literal(vec<string> $v): void {
  $c = C1::class;
  if ($v === vec[$c]) {
    expect_c1($v[0]);
  }
}

function sound_vec_var(vec<string> $v, vec<class<C1>> $x): void {
  if ($v === $x) {
    expect_c1($v[0]);
  }
}

function sound_dict(dict<string, string> $d, dict<string, class<C1>> $x): void {
  if ($d === $x) {
    expect_c1($d['k']);
  }
}

function sound_tuple((string, int) $t, (class<C1>, int) $x): void {
  if ($t === $x) {
    expect_c1($t[0]);
  }
}

function sound_tuple_variadic(
  (string, string...) $t,
  (class<C1>, class<C1>...) $x,
): void {
  if ($t === $x) {
    expect_c1($t[1]);
  }
}

function sound_shape(shape('c' => string) $s, shape('c' => class<C1>) $x): void {
  if ($s === $x) {
    expect_c1($s['c']);
  }
}

function sound_optional_shape_field(
  shape(?'c' => string) $s,
  shape(?'c' => class<C1>) $x,
): void {
  if ($s === $x) {
    expect_c1(Shapes::at($s, 'c'));
  }
}

function sound_open_shape(
  shape('c' => string, ...) $s,
  shape('c' => class<C1>, ...) $x,
): void {
  if ($s === $x) {
    expect_c1($s['c']);
  }
}

function sound_splat_shape<T as shape(...)>(T $a, mixed $s): void {
  $c = C1::class;
  if ($s === shape(...$a, 'c' => $c)) {
    expect_c1($s['c']);
  }
}

function sound_nested_vec(vec<vec<string>> $v, vec<vec<class<C1>>> $x): void {
  if ($v === $x) {
    expect_c1($v[0][0]);
  }
}

function sound_vec_of_shapes(
  vec<shape('c' => string)> $v,
  vec<shape('c' => class<C1>)> $x,
): void {
  if ($v === $x) {
    expect_c1($v[0]['c']);
  }
}

function sound_vec_or_dict(vec<string> $v, vec_or_dict<class<C1>> $x): void {
  if ($v === $x) {
    foreach ($v as $e) {
      expect_c1($e);
    }
  }
}

function sound_keyed_container_operand(
  vec<string> $v,
  KeyedContainer<int, class<C1>> $x,
): void {
  if ($v === $x) {
    expect_c1($v[0]);
  }
}

function sound_keyed_container_local(
  KeyedContainer<int, string> $kc,
  vec<class<C1>> $x,
): void {
  if ($kc === $x) {
    foreach ($kc as $e) {
      expect_c1($e);
    }
  }
}

function sound_loose_vec(vec<string> $v, vec<class<C1>> $x): void {
  if ($v == $x) {
    expect_c1($v[0]); // error: `==` does not refine
  }
}
