<?hh
<<file:__EnableUnstableFeatures('shape_and_tuple_destructuring')>>

class Keys {
  <<__ShapeKey>>
  const string A = 'same';
  <<__ShapeKey>>
  const string B = 'same';
}

function runtime_collision_in_pattern(dynamic $row): void {
  shape(Keys::A => $a, ?Keys::B => $b, ...) = $row;
}

function repeated_key_in_pattern(dynamic $row): void {
  shape('same' => $a, 'same' => $b, ...) = $row;
}

function nested_pattern_collision(dynamic $row): void {
  shape('outer' => shape(Keys::A => $a, Keys::B => $b, ...), ...) = $row;
}

function runtime_collision_in_rhs(): void {
  shape(Keys::A => $a, Keys::B => $b) = shape(Keys::A => 1, Keys::B => 2);
}
