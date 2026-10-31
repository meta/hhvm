<?hh

class Base {
  const string KEY = 'base_key';
}

class Child extends Base {
  <<__ShapeKey>>
  const string CHILD_KEY = 'child_key';

  public function test_parent_const(): shape(parent::KEY => int) {
    return shape(parent::KEY => 42);
  }

  public function test_static_const(): shape(static::CHILD_KEY => int) {
    return shape(static::CHILD_KEY => 42);
  }

  public function test_self_const(): shape(self::CHILD_KEY => int) {
    return shape(self::CHILD_KEY => 42);
  }
}
