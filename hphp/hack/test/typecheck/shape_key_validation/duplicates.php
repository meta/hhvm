<?hh

class Keys {
  <<__ShapeKey>>
  const string A = 'same';
  <<__ShapeKey>>
  const string B = 'same';
}

class InheritedKeys extends Keys {
  <<__ShapeKey>>
  const string C = 'same';
}

enum E: int as int {
  A = 1;
  B = 1;
}

type ClassCollision = shape(
  Keys::A => int,
  Keys::B => string,
  Keys::B => bool,
  Keys::A => float,
);
type InheritedCollision = shape(InheritedKeys::A => int, InheritedKeys::C => int);
type EnumCollision = shape(E::A => int, E::B => string);
type LiteralCollision = shape('same' => int, 'same' => string);

function duplicate_expression(): void {
  $_ = shape(Keys::A => 1, Keys::B => 2);
}
