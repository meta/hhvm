<?hh

trait T {
  <<__ShapeKey>>
  const string KEY = 'key';

  public function idx(shape(...) $s): void {
    Shapes::idx($s, self::KEY);
  }

  public function at(shape(...) $s): void {
    Shapes::at($s, nameof self);
  }

  public function exists(shape(...) $s): void {
    Shapes::keyExists($s, nameof self);
  }

  public function remove(shape(...) $s): void {
    Shapes::removeKey(inout $s, nameof self);
  }

  public function put(shape(...) $s): void {
    Shapes::put($s, nameof self, 42);
  }
}
