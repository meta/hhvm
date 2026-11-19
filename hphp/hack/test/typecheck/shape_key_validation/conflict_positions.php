<?hh

class ConflictKeys {
  <<__ShapeKey>>
  const string KEY = 'key';
  <<__ShapeKey>>
  const string OTHER = 'other';
}

type RepeatedLiteral = shape(
  'key' => int,
  'other' => int,
  'key' => string,
  'key' => bool,
);

type RepeatedConstant = shape(
  ConflictKeys::KEY => int,
  ConflictKeys::OTHER => int,
  ConflictKeys::KEY => string,
  ConflictKeys::KEY => bool,
);

function repeated_expressions(): void {
  $_ = shape(
    'key' => 1,
    'other' => 2,
    'key' => 'duplicate',
    'key' => true,
  );
  $_ = shape(
    ConflictKeys::KEY => 1,
    ConflictKeys::OTHER => 2,
    ConflictKeys::KEY => 'duplicate',
    ConflictKeys::KEY => true,
  );
}
