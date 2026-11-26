<?hh

function test(Traversable<int> $e) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(vec($e));
}

function test2(Traversable<int> $e) : void {
  $v = vec($e);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($v);
}

function foo<T>(vec<T> $v): T {
  return $v[0];
}

function test3(vec<int> $v): dynamic {
  return \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(foo($v));
}

function test4(vec<int> $v): dynamic {
  $e = foo($v);
  return \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($e);
}
