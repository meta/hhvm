<?hh

type OptionalField = shape(?'value' => int);
type OpenShape = shape('value' => int, ...);
newtype OpaqueAlias = int;
type GenericAlias<Tk, Tv> = (Tk, Tv);
type InstantiatedAlias = GenericAlias<int, string>;

<<__EntryPoint>>
function main(): void {
  $ts = HH\type_structure(OptionalField::class)['fields']['value'];
  invariant(
    array_key_exists('optional_shape_field', $ts),
    'missing optional_shape_field',
  );
  HH\FIXME\UNSAFE_CAST<mixed, HH\TypeStructure<mixed>>($ts);

  $ts = HH\type_structure(OpenShape::class);
  invariant(
    array_key_exists('allows_unknown_fields', $ts),
    'missing allows_unknown_fields',
  );
  HH\FIXME\UNSAFE_CAST<mixed, HH\TypeStructure<mixed>>($ts);

  $ts = HH\type_structure(OpaqueAlias::class);
  invariant(array_key_exists('opaque', $ts), 'missing opaque');
  HH\FIXME\UNSAFE_CAST<mixed, HH\TypeStructure<mixed>>($ts);

  $ts = HH\type_structure(GenericAlias::class);
  invariant(array_key_exists('typevars', $ts), 'missing typevars');
  invariant($ts['typevars'] === 'Tk,Tv', 'expected comma-separated typevars');
  HH\FIXME\UNSAFE_CAST<mixed, HH\TypeStructure<mixed>>($ts);

  $ts = HH\type_structure(InstantiatedAlias::class);
  invariant(array_key_exists('typevar_types', $ts), 'missing typevar_types');
  HH\FIXME\UNSAFE_CAST<mixed, HH\TypeStructure<mixed>>($ts);

  echo "done\n";
}
