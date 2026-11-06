<?hh

class C {
  const type T = int;
}

function generic_types_of<T>(TypeStructure<T> $ts): vec<mixed> {
  return $ts['generic_types'];
}

<<__EntryPoint>>
function main(): void {
  generic_types_of(type_structure(C::class, 'T'));
}
