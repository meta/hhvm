<?hh

function dyn(): dynamic { return \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>("4"); }

function my_vec<T>(T $a, T $b) : vec<T> {
  return vec[$a, $b];
}

function test1(): vec<dynamic> {
  return vec[\HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(1), dyn()];
}

function test2(): vec<dynamic> {
  return my_vec(\HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(1), dyn());
}

function test3():void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, vec<dynamic>>(vec[5]);
}
