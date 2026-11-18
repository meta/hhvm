<?hh

class Key {
  <<__ShapeKey>>
  const string FIELD = 'field';
}

function symbolic_lookup(shape(Key::FIELD => int) $s): int {
  return $s[Key::FIELD];
}

function literal_lookup(shape(Key::FIELD => int) $s): int {
  return $s['field'];
}
