<?hh

namespace ShapeKeys;

use type ShapeKeys\First as Alias;

class First {
  <<__ShapeKey>>
  const string KEY = 'same';
  <<__ShapeKey>>
  const string OTHER = 'same';
}

class Second {
  <<__ShapeKey>>
  const string KEY = 'different';
}

type DuplicateRuntimeKeys = shape(First::KEY => int, First::OTHER => int);
type DistinctClassConstantKeys = shape(First::KEY => int, Second::KEY => int);
type DistinctNameofKeys = shape(nameof First => int, nameof Second => int);

function symbolic_duplicates(): void {
  $_ = shape(First::KEY => 1, First::OTHER => 2);
  $_ = shape(First::KEY => 1, Alias::KEY => 2);
  $_ = shape(nameof First => 1, nameof Alias => 2);
}
