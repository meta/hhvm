<?hh

type MyShape = shape(
  ?'a' => int,
  'b' => bool,
);

function test(TypeStructure<MyShape> $type_structure): void {
  hh_show($type_structure['kind']);
  hh_show($type_structure['alias'] ?? null);
  hh_show($type_structure['name'] ?? null);
  hh_show($type_structure['fields']);
}
