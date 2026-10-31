<?hh

class TestClass {
  <<__ShapeKey>>
  const string KEY = 'key';

  const type TClassType = shape(
    self::KEY => int,
  );
}
