<?hh

class C {
  <<__ShapeKey>>
  const string KEY = 'KEY';

  const mixed X = shape(
    0 => 0,
    's' => 's',
    nameof C => C::class,
    C::KEY => C::KEY,
  );
}
