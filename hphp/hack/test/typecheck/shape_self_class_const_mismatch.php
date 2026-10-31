<?hh

type MyShape = shape(MyClass::KEY_NAME => int);

abstract class MyClass {
  <<__ShapeKey>>
  const string KEY_NAME = 'id1';
  public function example(MyShape $shape): int {
    return $shape[self::KEY_NAME];
  }
}

abstract class OtherClass {
  <<__ShapeKey>>
  const string KEY_NAME = 'id1';
  public function example(MyShape $shape): int {
    return $shape[self::KEY_NAME];
  }
}
