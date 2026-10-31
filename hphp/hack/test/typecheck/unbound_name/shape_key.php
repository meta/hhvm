<?hh

class A {
  <<__ShapeKey>>
  const string KEY = "KEY";
}

type my_shape = shape(
  A::KEY => string,
);

type my_shape2 = shape(
  "HELLO" => shape(
    NotFound::KEY => string,
  ),
);
