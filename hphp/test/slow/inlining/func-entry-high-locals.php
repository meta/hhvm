<?hh
class State { public static mixed $value = 0; }
// The optimized func-entry side exit must preserve inputs above local 61.
<<__NEVER_INLINE>>
function source(): mixed { return __hhvm_intrinsics\launder_value(State::$value); }
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
  mixed $p64
): mixed {
  $d = $p0 + 1;
  $e = $p1 + 1;
  $sum = $p1 + $p2 + $p3 + $p4 + $p5 + $p6 + $p7 + $p8 + $p9 + $p10 + $p11 + $p12 + $p13 + $p14 + $p15 + $p16 + $p17 + $p18 + $p19 + $p20 + $p21 + $p22 + $p23 + $p24 + $p25 + $p26 + $p27 + $p28 + $p29 + $p30 + $p31 + $p32 + $p33 + $p34 + $p35 + $p36 + $p37 + $p38 + $p39 + $p40 + $p41 + $p42 + $p43 + $p44 + $p45 + $p46 + $p47 + $p48 + $p49 + $p50 + $p51 + $p52 + $p53 + $p54 + $p55 + $p56 + $p57 + $p58 + $p59 + $p60 + $p61 + $p62 + $p63 + $p64;
  $a = HH\ReifiedGenerics\get_type_structure<T>();
  $b = __hhvm_intrinsics\launder_value(42);
  $c = __hhvm_intrinsics\launder_value(43);
  return vec[$d, $e, $sum, $p0, $p1, $p62, $p63, $p64, $a, $b, $c];
}
<<__NEVER_INLINE>>
function caller(int $seed): mixed { return target<int>(source(), 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015, 1016, 1017, 1018, 1019, 1020, 1021, 1022, 1023, 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031, 1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041, 1042, 1043, 1044, 1045, 1046, 1047, 1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055, 1056, 1057, 1058, 1059, 1060, 1061, $seed, $seed + 1, $seed + 2); }
<<__EntryPoint>>
function main(): void {
  $success = false;
  $n = apc_fetch('review_request', inout $success);
  $n = $n is int ? $n + 1 : 1;
  apc_store('review_request', $n);
  $v = __hhvm_intrinsics\launder_value($n <= 2 ? 123 : 123.5);
  State::$value = $v;
  $seed = $n <= 2 ? 1062 : 2062;
  $expected = vec[$v + 1, 1002, 62891 + 3 * $seed + 3, $v, 1001, $seed, $seed + 1, $seed + 2, HH\ReifiedGenerics\get_type_structure<int>(), 42, 43];
  $failures = 0;
  for ($i = 0; $i < 100; ++$i) {
    if (caller($seed) !== $expected) ++$failures;
  }
  echo "$failures\n";
}
