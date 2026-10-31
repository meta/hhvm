<?hh

trait Keys {
  <<__ShapeKey>>
  const string KEY = 'key';

  public function explicit_key(): shape(Keys::KEY => int) {
    return shape(Keys::KEY => 1);
  }
}

trait InheritedKey {
  use Keys;

  public function make(): shape(...) {
    return shape(self::KEY => 1);
  }
}

abstract class Base {
  const string KEY = 'key';
}

trait RequiredClassKey {
  require extends Base;

  public function make(): shape(...) {
    return shape(self::KEY => 1);
  }
}

interface I {
  <<__ShapeKey>>
  const string KEY = 'key';
}

trait RequiredInterfaceKey {
  require implements I;

  public function make(): shape(...) {
    return shape(self::KEY => 1);
  }
}
