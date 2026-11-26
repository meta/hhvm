<?hh

class C {}

function test_prim(mixed $m, nonnull $n, C $c) : void {
  //hh_log_level('sub', 2);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($c);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($m);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($n);
}

function test_contain((string, C) $t, vec<C> $v, dict<string, C> $di,
                      darray<arraykey, C> $da,
                      varray<C> $va)
  : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($t);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($v);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($di);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($da);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($va);
}

function test_shape(shape('x' => int, ?'y' => vec<C>) $s) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($s);
}


function test_container_context(vec<C> $v,
                                dict<int, C> $d,
                                darray<arraykey, C> $da,
                                varray<C> $va) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, vec<dynamic>>($v);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dict<arraykey, dynamic>>($d);
}

function test_shape_context(shape('x' => int, ?'y' => C) $s) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, shape('x' => dynamic, ?'y' => dynamic)>($s);
}

class V1<<<__RequireDynamic>> T> {};
class V2<<<__RequireDynamic>> +T> {};

function test_class_context(V1<int> $v1, V2<C> $v2) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, V1<dynamic>>($v1);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, V2<dynamic>>($v2);
}

function test_union(int $i, C $c, bool $b) : void {
  if ($b) {
    $x = $i;
  } else {
    $x = $c;
  }
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($x);
}
