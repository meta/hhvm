<?hh

namespace NS1\NS2;

class C {
  <<__ShapeKey>>
  const string KEY = 'KEY';

  const type TInt = int;
  const type TShape = shape(
    self::KEY => self::TInt,
  );

  public function f(self::TInt $x): void {}
}
