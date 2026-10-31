<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

type my_shape = shape(
  'x' => int,
  ...
);

class C {
  <<__ShapeKey>>
  const int KEY1 = 1;
  <<__ShapeKey>>
  const int KEY2 = 2;
}

function test_good(my_shape $s): void {
  hh_show(Shapes::toDict($s));
  hh_show(Shapes::toDict(shape()));
  hh_show(Shapes::toDict(shape('x' => 4, 'y' => 'aaa')));
  hh_show(Shapes::toDict(shape(C::KEY1 => 4, C::KEY2 => 'aaa')));
  $s = shape();
  $s['x'] = 3;
  hh_show(Shapes::toDict($s));
}

function test_bad(): darray<string, num> {
  return Shapes::toDict(shape('x' => 42, 'y' => 3.14));
}
