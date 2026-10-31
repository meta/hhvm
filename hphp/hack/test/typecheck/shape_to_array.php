<?hh

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

function expect<T>(T $x):void { }

function test(my_shape $s): void {
  expect<darray<arraykey,mixed>>(Shapes::toArray($s));
  expect<darray<int,bool>>(Shapes::toArray(shape()));
  expect<darray<string,arraykey>>(Shapes::toArray(shape('x' => 4, 'y' => 'aaa')));
  expect<darray<int,arraykey>>(Shapes::toArray(shape(C::KEY1 => 4, C::KEY2 => 'aaa')));
  $s = shape();
  $s['x'] = 3;
  expect<darray<string,int>>(Shapes::toArray($s));
}
