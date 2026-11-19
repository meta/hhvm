<?hh
<<file:__EnableUnstableFeatures('shape_and_tuple_destructuring')>>

class Keys {
  <<__ShapeKey>>
  const string A = 'same';
  <<__ShapeKey>>
  const string B = 'same';
  <<__ShapeKey>>
  const string C = 'different';
}

function runtime_collision_in_pattern(dynamic $row): void {
  shape(Keys::A => $a, ?Keys::B => $b, ...) = $row;
}

function repeated_key_in_pattern(dynamic $row): void {
  shape(Keys::A => $a, Keys::A => $b, ...) = $row;
}

function nested_pattern_collision(dynamic $row): void {
  shape('outer' => shape(Keys::A => $a, Keys::B => $b, ...), ...) = $row;
  tuple(shape('same' => $c, ?'same' => $d, ...), ...) = $row;
  shape('outer' => tuple(shape(Keys::A => $e, ?Keys::B => $f, ...), ...), ...) = $row;
}

function foreach_value_collision(vec<nothing> $rows): void {
  foreach ($rows as shape(Keys::A => $a, ?Keys::B => $b, ...)) {}
}

function foreach_key_collision(KeyedIterator<nothing, nothing> $rows): void {
  foreach ($rows as shape('same' => $a, 'same' => $b, ...) => $value) {}
}

function runtime_collision_in_rhs(): void {
  shape(Keys::A => $a, Keys::B => $b) = shape(Keys::A => 1, Keys::B => 2);
}

function non_colliding_patterns(dynamic $row): void {
  shape(Keys::A => $a, ?Keys::C => $c, ...) = $row;
  shape('left' => shape('same' => $l, ...),
        'right' => shape('same' => $r, ...), ...) = $row;
  tuple(shape(Keys::A => $x, ...), shape(Keys::B => $y, ...), ...) = $row;
  tuple(shape(nameof Keys => $named, ...), ...) = $row;
}
