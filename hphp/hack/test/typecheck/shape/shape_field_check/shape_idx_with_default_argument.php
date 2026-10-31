<?hh

class TestClass {
  <<__ShapeKey>>
  const string ID = 'id';
  const string X = 'x';

  const type TClassType = shape(
    self::ID => int,
    /* HH_FIXME[4050] mixing literal and constant shape keys */
    'x' => int,
  );

  public function test(self::TClassType $s): void {
    Shapes::idx($s, 'xy', 0);
  }
}
