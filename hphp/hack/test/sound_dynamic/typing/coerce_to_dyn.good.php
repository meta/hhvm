<?hh

<<__SupportDynamicType>>
class C {
  public function m() : void {}
}

enum E1: int {
  CONST1 = 1;
}

enum E2: int as int {
  CONST2 = 2;
}

enum E3: string {
  CONST3 = "a";
}

enum E4: string as string {
  CONST4 = "b";
}

function test_enum_type(E4 $e) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($e);
}

function test_prim(bool $b, int $i, float $f, num $n, string $s, arraykey $ak, resource $r)
  : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($b);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($i);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($n);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($s);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($r);
  $c = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(new C());
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($c->m());
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(null);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($ak);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(E1::CONST1);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(E2::CONST2);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(E3::CONST3);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(E4::CONST4);
}

function test_contain((string, int) $t, vec<int> $v, dict<string, int> $di,
                      keyset<arraykey> $ks, darray<arraykey, vec<C>> $da,
                      varray<int> $va, ?int $oi)
  : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($t);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($v);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($di);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($ks);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($da);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($va);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($oi);
}

function test_shape(shape('x' => int, ?'y' => vec<C>) $s) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($s);
}

<<__SupportDynamicType>>
class D<T> {}

<<__SupportDynamicType>>
class E {}

function test_class(C $c, D<int> $di, D<E> $de) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($c);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($di);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($de);
}

function test_container_context(vec<int> $v,
                                dict<int, C> $d,
                                ?int $o) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, vec<dynamic>>($v);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dict<arraykey, dynamic>>($d);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, ?dynamic>($o);
}

function test_shape_context(shape('x' => int, ?'y' => vec<C>) $s) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, shape('x' => dynamic, ?'y' => dynamic)>($s);
}

function test_union(int $i, C $c, bool $b) : void {
  if ($b) {
    $x = $i;
  } else {
    $x = $c;
  }
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($x);
}

function test_inter(int $i, bool $b) : void {
  if ($i is E) {
    \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($i);
  }
}

function test_expression_helper() : int {
  return 10;
}

function test_expression(int $i) : void {
  $x = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(test_expression_helper());
  $y = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($i + 1);
  $z = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(1);
  $m = (\HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, int>(1)) + 1;
  $n = 1 + (\HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, int>(1));
  $o = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, int>(1) + 1;
}
