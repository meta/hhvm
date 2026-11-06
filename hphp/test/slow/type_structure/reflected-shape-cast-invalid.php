<?hh

function cast_type_structure(mixed $ts): void {
  HH\FIXME\UNSAFE_CAST<mixed, HH\TypeStructure<mixed>>($ts);
}

<<__EntryPoint>>
function main(): void {
  cast_type_structure(
    dict['kind' => HH\TypeStructureKind::OF_INT, 'optional_shape_field' => 'true'],
  );
  cast_type_structure(
    dict[
      'kind' => HH\TypeStructureKind::OF_SHAPE,
      'fields' => dict[],
      'allows_unknown_fields' => 'true',
    ],
  );
  cast_type_structure(
    dict['kind' => HH\TypeStructureKind::OF_INT, 'opaque' => 'true'],
  );
  cast_type_structure(
    dict['kind' => HH\TypeStructureKind::OF_INT, 'typevars' => vec['T']],
  );
  cast_type_structure(
    dict['kind' => HH\TypeStructureKind::OF_INT, 'typevar_types' => vec[]],
  );
  cast_type_structure(
    dict['kind' => HH\TypeStructureKind::OF_INT, 'client_metadata' => true],
  );

  echo "done\n";
}
