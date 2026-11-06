<?hh

class C<T> {
  const type Tthis = this;
}

function name_of(TypeStructure<int> $ts): string {
  return $ts['name'];
}

function alias_of(TypeStructure<int> $ts): string {
  return $ts['alias'];
}

function generic_types_of_generic<T>(TypeStructure<T> $ts): vec<mixed> {
  return $ts['generic_types'];
}

function generic_types_of_class(
  TypeStructure<C<int>> $ts,
): TypeStructure<int> {
  return $ts['generic_types'][0];
}

function generic_types_of_this(): mixed {
  return type_structure(C::class, 'Tthis')['generic_types'];
}
