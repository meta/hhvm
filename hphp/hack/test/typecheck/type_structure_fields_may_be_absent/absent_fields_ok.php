<?hh

class C<T> {}

function kind_of<T>(TypeStructure<T> $ts): TypeStructureKind {
  return $ts['kind'];
}

function classname_of<T as C<int>>(TypeStructure<T> $ts): classname<T> {
  return $ts['classname'];
}

function elem_types_of(TypeStructure<(int, string)> $ts): mixed {
  return $ts['elem_types'];
}

function generic_types_or_empty<T>(TypeStructure<T> $ts): vec<mixed> {
  return $ts['generic_types'] ?? vec[];
}

function generic_types_idx(TypeStructure<C<int>> $ts): mixed {
  return Shapes::idx($ts, 'generic_types');
}
