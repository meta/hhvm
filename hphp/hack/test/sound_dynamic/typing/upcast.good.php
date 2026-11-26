<?hh

function f(int $x) : void {
  $y = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($x);
  hh_expect_equivalent<dynamic>($y);
}

function g<T as int>(T $x) : void {
  $y = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($x);
  hh_expect_equivalent<dynamic>($y);
}

function h(vec<int> $x) : void {
  $y = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, vec<dynamic>>($x);
  hh_expect_equivalent<vec<dynamic>>($y);
  $z = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($x);
  hh_expect_equivalent<dynamic>($z);
}

function k() : void {
  $z = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(1);
  hh_expect_equivalent<dynamic>($z);
}

function j<T as int>(T $x) : void {
  $z = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($x + 1);
  hh_expect_equivalent<dynamic>($z);
}

class C {
  public dynamic $d = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(3);
}
