<?hh


<<__SupportDynamicType>>
class C {}

function f(
  ~vec<int> $x,
  ~vec<string> $y,
  dynamic $d,
  vec<int> $z,
  vec<string> $w,
  vec<C> $c,
  supportdyn<(function(vec<int>, vec<string>): bool)> $g,
  supportdyn<(function(vec<C>, vec<string>): bool)> $h,
  supportdyn<mixed> $m,
): ~bool {
  $r1 = $g($x, $y);
  hh_expect_equivalent<~bool>($r1);
  $r2 = $g($d, $y);
  hh_expect_equivalent<~bool>($r2);
  $r3 = $g($z, $y);
  hh_expect_equivalent<~bool>($r3);
  $r4 = $g($z, $w);
  hh_expect_equivalent<bool>($r4);
  $r5 = $h($c, $w);
  hh_expect_equivalent<bool>($r5);
  $r6 = (\HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($m))(1);
  hh_expect_equivalent<dynamic>($r6);
  return $r1;
}
