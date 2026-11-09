<?hh
<<file:__EnableUnstableFeatures(
  'shape_splat_concrete',
  'shape_splat_type_parameters',
  'union_intersection_type_hints',
)>>

class TypeTextValue {}
class TypeTextContexts {
  const ctx C = [write_props];
}

type ComplexBase = shape('base' => int);
type ComplexOther = shape('other' => string);

function optional_tuple(
  (int, optional string, optional bool) $p,
): (int, optional string, optional bool) {
  return $p;
}

function variadic_tuple((int, string...) $p): (int, string...) {
  return $p;
}

function combined_tuple(
  (int, optional string, bool...) $p,
): (int, optional string, bool...) {
  return $p;
}

function only_variadic_tuple((int...) $p): (int...) {
  return $p;
}

function tuple_splat<T as (mixed...)>(
  (int, ...T) $p,
): (int, ...T) {
  return $p;
}

function full_function(
  (readonly function(
    readonly TypeTextValue,
    inout vec<int>,
    optional string,
    bool...
  )[globals, write_props]: readonly TypeTextValue) $p,
): (readonly function(
  readonly TypeTextValue,
  inout vec<int>,
  optional string,
  bool...
)[globals, write_props]: readonly TypeTextValue) {
  return $p;
}

function pure_function(
  (function(int)[]: void) $p,
): (function(int)[]: void) {
  return $p;
}

function static_context(
  (function()[TypeTextContexts::C]: void) $p,
): (function()[TypeTextContexts::C]: void) {
  return $p;
}

function no_context(
  (function(int): void) $p,
): (function(int): void) {
  return $p;
}

function nested_shape(
  shape(
    ...shape(...ComplexBase, 'tuple' => (int, optional string, bool...)),
    ?'callback' => ?(readonly function(
      readonly TypeTextValue,
      inout vec<int>,
      optional string,
      bool...
    )[globals, write_props]: readonly TypeTextValue),
    (ComplexBase | ComplexOther)...
  ) $p,
): shape(
  ...shape(...ComplexBase, 'tuple' => (int, optional string, bool...)),
  ?'callback' => ?(readonly function(
    readonly TypeTextValue,
    inout vec<int>,
    optional string,
    bool...
  )[globals, write_props]: readonly TypeTextValue),
  (ComplexBase | ComplexOther)...
) {
  return $p;
}

class ComplexTypeTextHolder<T as (mixed...)> {
  public (int, optional string, bool...) $tuple;
  public (int, ...T) $tuple_splat;
  public (readonly function(
    readonly TypeTextValue,
    inout vec<int>,
    optional string,
    bool...
  )[globals, write_props]: readonly TypeTextValue) $callback;
  public shape(
    ...shape(...ComplexBase, 'tuple' => (int, optional string, bool...)),
    ?'callback' => ?(readonly function(
      readonly TypeTextValue,
      inout vec<int>,
      optional string,
      bool...
    )[globals, write_props]: readonly TypeTextValue),
    (ComplexBase | ComplexOther)...
  ) $nested;
}

<<__EntryPoint>>
function main_complex_type_text(): void {
  foreach (vec[
    'optional_tuple',
    'variadic_tuple',
    'combined_tuple',
    'only_variadic_tuple',
    'tuple_splat',
    'full_function',
    'pure_function',
    'static_context',
    'no_context',
    'nested_shape',
  ] as $name) {
    $reflection = new ReflectionFunction($name);
    echo $name.': '.$reflection->getParameters()[0]->getTypeText().
      ' -> '.$reflection->getReturnTypeText()."\n";
  }
  foreach (vec['tuple', 'tuple_splat', 'callback', 'nested'] as $name) {
    echo 'property '.$name.': '.
      (new ReflectionProperty(ComplexTypeTextHolder::class, $name))
        ->getTypeText()."\n";
  }
}
