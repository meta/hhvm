<?hh

class C {
  const type T = int;
}

function alias_of(TypeStructure<int> $ts): string {
  return $ts['alias'];
}

<<__EntryPoint>>
function main(): void {
  alias_of(type_structure(C::class, 'T'));
}
