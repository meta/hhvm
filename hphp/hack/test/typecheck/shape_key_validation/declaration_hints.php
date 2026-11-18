<?hh

class Keys {
  <<__ShapeKey>>
  const string A = 'same';
  <<__ShapeKey>>
  const string B = 'same';
}

function parameter_and_return(
  shape(Keys::A => int, Keys::B => int) $value,
): shape(Keys::A => int, Keys::B => int) {
  return $value;
}

const shape(Keys::A => int, Keys::B => int) GLOBAL_FIELDS =
  shape(Keys::A => 1, Keys::B => 2);

class DeclarationFields {
  public shape(Keys::A => int, Keys::B => int) $fields =
    shape(Keys::A => 1, Keys::B => 2);

  const shape(Keys::A => int, Keys::B => int) FIELDS =
    shape(Keys::A => 1, Keys::B => 2);

  const type Fields = shape(Keys::A => int, Keys::B => int);

  public function method(shape(Keys::A => int, Keys::B => int) $value): void {
    $_ = function(shape(Keys::A => int, Keys::B => int) $value): void {};
  }
}

function expression_hints(mixed $value): void {
  $_ = $value is shape(Keys::A => int, Keys::B => int);
  $_ = $value as shape(Keys::A => int, Keys::B => int);
}
