<?hh
<<file:__EnableUnstableFeatures('shape_and_tuple_destructuring')>>

class Keys {
  <<__ShapeKey>>
  const string A = 'same';
  <<__ShapeKey>>
  const string B = 'same';
}

function symbolic_duplicates(dynamic $row): void {
  shape('same' => $a, ?'same' => $b, ...) = $row;
  shape(Keys::A => $c, Keys::A => $d, ...) = $row;
}

function runtime_duplicates(dynamic $row): void {
  shape(Keys::A => $a, ?Keys::B => $b, ...) = $row;
}
