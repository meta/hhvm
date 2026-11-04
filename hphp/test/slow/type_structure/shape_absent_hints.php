<?hh
<<file:__EnableUnstableFeatures('shape_splat_concrete')>>

type AbsentShape = shape(absent 'x');

class AbsentHints {
  const type Absent = shape(absent 'x');
  const type Nothing = shape(?'x' => nothing);
}

function roundtrip(shape(absent 'x') $s): shape(absent 'x') {
  return $s;
}

function roundtrip_alias(AbsentShape $s): AbsentShape {
  return $s;
}

<<__EntryPoint>>
function main(): void {
  var_dump(roundtrip(shape()) === shape());
  var_dump(roundtrip_alias(shape()) === shape());
  var_dump(
    type_structure(AbsentHints::class, 'Absent') ===
    type_structure(AbsentHints::class, 'Nothing'),
  );

  var_dump(
    HH\ReifiedGenerics\get_type_structure<shape(absent 'x')>() ===
    HH\ReifiedGenerics\get_type_structure<shape(?'x' => nothing)>(),
  );
}
