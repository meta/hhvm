<?hh

class KeyPrefix {
  const string PREFIX = 'prefix_';
}

enum ComputedKeys: string as string {
  FORWARDED = KeyPrefix::PREFIX;
  COMPUTED = 'pre'.'fix';
}

<<__AllowUncheckedEnumValues>>
enum OptedOutKeys: string as string {
  Y = KeyPrefix::PREFIX;
}

type ComputedShape = shape(ComputedKeys::FORWARDED => int);
type OptedOutShape = shape(OptedOutKeys::Y => int);

function accesses(ComputedShape $computed): void {
  $_ = Shapes::idx($computed, ComputedKeys::COMPUTED);
  $_ = $computed[ComputedKeys::FORWARDED];
}
