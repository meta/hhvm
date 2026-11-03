<?hh
<<file:__EnableUnstableFeatures('shape_and_tuple_destructuring', 'shape_field_punning')>>

function destructure(inout int $_): void {
  shape('x' => $_) = shape('x' => '');
  tuple($_) = tuple('');
  shape($_) = shape('_' => '');
}

function discard(): void {
  shape('x' => $_) = shape('x' => '');
  tuple($_) = tuple('');
}

function wildcard(inout int $_): void {
  shape('x' => _) = shape('x' => '');
  tuple(_) = tuple('');
}

// TODO(T68713970): Check these writes if match-pattern bindings become supported.
// Both forms currently fail Parsing[1002].
// match ($value) { $_ => {} }
// match ($value) { $_: SomeClass => {} _ => {} }
