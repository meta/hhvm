<?hh
<<file:__EnableUnstableFeatures('shape_and_tuple_destructuring')>>

abstract class Keys {
  <<__ShapeKey>>
  const string A = 'a';
  const string Unmarked = 'unmarked';
  const bool BadType = true;
  abstract const string Abstract;
}

class OtherKeys {
  <<__ShapeKey>>
  const string A = 'other';
}

function invalid_constants(dynamic $row): void {
  shape(Keys::BadType => $a, ...) = $row;
  shape(Keys::Abstract => $b, ?Keys::Abstract => $c, ...) = $row;
}

function nested_invalid_constants(dynamic $row): void {
  shape('outer' => tuple(shape(?Keys::Missing => $a, ...), ...), ...) = $row;
}

function mixed_keys(dynamic $row): void {
  shape('literal' => $a, Keys::A => $b, ...) = $row;
  shape(Keys::A => $c, ?'literal' => $d, ...) = $row;
  shape(Keys::A => $e, ?OtherKeys::A => $f, ...) = $row;
}

function foreach_invalid_keys(vec<nothing> $rows): void {
  foreach ($rows as tuple(shape(?Keys::Unmarked => $a, ...), ...)) {}
}
