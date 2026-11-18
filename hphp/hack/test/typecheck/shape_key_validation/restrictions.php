<?hh

class Keys {
  <<__ShapeKey>>
  const string A = 'a';
  <<__ShapeKey>>
  const int B = 1;
}

class MoreKeys {
  <<__ShapeKey>>
  const string A = 'b';
}

enum E: string as string {
  A = 'a';
}

enum F: string as string {
  A = 'b';
}

type DifferentClasses = shape(Keys::A => int, MoreKeys::A => string);
type LiteralFirst = shape('a' => int, Keys::A => string);
type ConstantFirst = shape(Keys::A => int, 'a' => string);
type DifferentTypes = shape(Keys::A => int, Keys::B => string);
type DifferentEnums = shape(E::A => int, F::A => string);
type DifferentNames = shape(nameof Keys => int, nameof MoreKeys => string);
