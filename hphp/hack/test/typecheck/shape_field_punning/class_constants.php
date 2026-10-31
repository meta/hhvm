<?hh
<<file:__EnableUnstableFeatures('shape_field_punning')>>

// Note: Hack does not allow mixing class constants with literal string
// field names in shapes. These tests document the expected error behavior.

class MyClass {
  <<__ShapeKey>>
  const string FIELD_KEY = 'key';
}

// Error: Cannot mix class constants with literal field names (punning creates literals)
function test_class_const_with_punning_error(): void {
  $other = 42;
  // This errors because $other becomes literal 'other' which can't mix with class constant
  $s = shape(MyClass::FIELD_KEY => 'value', $other);
}

// Error: Punning before class constant also errors
function test_punning_before_class_const_error(): void {
  $first = 1;
  // $first becomes literal 'first', can't mix with class constant
  $s = shape($first, MyClass::FIELD_KEY => 'value');
}
