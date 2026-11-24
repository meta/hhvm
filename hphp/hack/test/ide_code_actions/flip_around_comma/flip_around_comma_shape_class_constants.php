<?hh

class Keys {
  <<__ShapeKey>>
  const string FIRST = 'first';
  <<__ShapeKey>>
  const string SECOND = 'second';
}

function values(): void {
  shape(Keys::FIRST => 1, /*range-start*//*range-end*/Keys::SECOND => 2);
}
