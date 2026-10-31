<?hh

type MyShape = shape(Foo::KEY_NAME => int);

abstract class Foo {
  <<__ShapeKey>>
  const string KEY_NAME = 'id1';
  public function printVal(MyShape $shape): int {
    return $shape[self::KEY_NAME];
  }

  public function printValLocal(): int {
    $shape = shape(self::KEY_NAME => 4);
    return $shape[self::KEY_NAME];
  }
}
