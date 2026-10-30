<?hh
<<file:__EnableUnstableFeatures(
  'shape_splat_concrete',
  'shape_splat_type_parameters',
  'union_intersection_type_hints',
)>>

type Base = shape('hidden' => int);
type Other = shape('other' => string);

function direct(
  shape(...Base, 'z' => bool) $p,
): shape(...Base, 'z' => bool) {
  return $p;
}

function only_splat(shape(...Base) $p): shape(...Base) {
  return $p;
}

function nested(
  vec<shape(...shape(...Base), 'z' => bool)> $p,
): vec<shape(...shape(...Base), 'z' => bool)> {
  return $p;
}

function union_splat(
  shape(...(Base | Other), 'z' => bool) $p,
): shape(...(Base | Other), 'z' => bool) {
  return $p;
}

function intersection_splat(
  shape(...(Base & Other), 'z' => bool) $p,
): shape(...(Base & Other), 'z' => bool) {
  return $p;
}

class Holder<T as shape(...)> {
  public shape(...Base, 'z' => bool, int...) $value;
  public shape(...T, T...) $generic;

  public static function method(
    shape(...Base, ...) $p,
  ): shape(...Base, ...) {
    return $p;
  }
}

<<__EntryPoint>>
function main(): void {
  foreach (
    vec['direct', 'only_splat', 'nested', 'union_splat', 'intersection_splat']
    as $name
  ) {
    $r = new ReflectionFunction($name);
    echo $name.': '.$r->getParameters()[0]->getTypeText().
      ' -> '.$r->getReturnTypeText()."\n";
  }
  $method = new ReflectionMethod(Holder::class, 'method');
  echo 'method: '.$method->getParameters()[0]->getTypeText().
    ' -> '.$method->getReturnTypeText()."\n";
  foreach (vec['value', 'generic'] as $name) {
    echo $name.': '.
      (new ReflectionProperty(Holder::class, $name))->getTypeText()."\n";
  }
}
