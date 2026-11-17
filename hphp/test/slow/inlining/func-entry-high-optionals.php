<?hh
class State { public static mixed $value = 0; }
<<__NEVER_INLINE>>
function source(): mixed {
  return __hhvm_intrinsics\launder_value(State::$value);
}
// Missing high positionals must not kill the counted inputs or generics.
<<__ALWAYS_INLINE>>
function target<reify T>(
  mixed $p0,
  mixed $p1,
  mixed $p2,
  mixed $p3,
  mixed $p4,
  mixed $p5,
  mixed $p6,
  mixed $p7,
  mixed $p8,
  mixed $p9,
  mixed $p10,
  mixed $p11,
  mixed $p12,
  mixed $p13,
  mixed $p14,
  mixed $p15,
  mixed $p16,
  mixed $p17,
  mixed $p18,
  mixed $p19,
  mixed $p20,
  mixed $p21,
  mixed $p22,
  mixed $p23,
  mixed $p24,
  mixed $p25,
  mixed $p26,
  mixed $p27,
  mixed $p28,
  mixed $p29,
  mixed $p30,
  mixed $p31,
  mixed $p32,
  mixed $p33,
  mixed $p34,
  mixed $p35,
  mixed $p36,
  mixed $p37,
  mixed $p38,
  mixed $p39,
  mixed $p40,
  mixed $p41,
  mixed $p42,
  mixed $p43,
  mixed $p44,
  mixed $p45,
  mixed $p46,
  mixed $p47,
  mixed $p48,
  mixed $p49,
  mixed $p50,
  mixed $p51,
  mixed $p52,
  mixed $p53,
  mixed $p54,
  mixed $p55,
  mixed $p56,
  mixed $p57,
  mixed $p58,
  mixed $p59,
  mixed $p60,
  mixed $p61,
  mixed $p62,
  mixed $p63,
  mixed $p64,
  mixed $p65 = null,
  mixed $p66 = null,
  mixed $p67 = null,
  mixed $p68 = null,
  mixed $p69 = null,
  mixed $p70 = null
): mixed {
  return vec[
    $p0 + 1, $p62, $p63, $p64, $p65, $p70,
    HH\ReifiedGenerics\get_type_structure<T>(),
  ];
}
<<__NEVER_INLINE>>
function caller(mixed $value): mixed {
  return target<int>(source(), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, $value, $value, $value);
}
<<__EntryPoint>>
function main(): void {
  $success = false;
  $n = apc_fetch('review_request', inout $success);
  $n = $n is int ? $n + 1 : 1;
  apc_store('review_request', $n);
  $v = __hhvm_intrinsics\launder_value($n <= 2 ? 123 : 123.5);
  State::$value = $v;
  $value = vec[$n];
  $expected = vec[
    $v + 1, $value, $value, $value, null, null,
    HH\ReifiedGenerics\get_type_structure<int>(),
  ];
  $failures = 0;
  for ($i = 0; $i < 100; ++$i) {
    if (caller($value) !== $expected) ++$failures;
  }
  echo "$failures\n";
}
