<?hh

final class Attr<T> implements HH\ClassAttribute {
  public function __construct(public T $value)[] {}
}

<<Attr('foo')>>
final class Target {}

// Attr::class fills the missing type argument with an inference variable, so
// getAttributeClass() returns the 'foo' attribute typed as Attr<int>.
function attribute_value(): int {
  $attr = (new ReflectionClass(Target::class))
    ->getAttributeClass(Attr::class);
  return $attr?->value ?? 0;
}
