<?hh

final class Attr<T> implements HH\ClassAttribute {
  public function __construct(public T $value)[] {}
}

<<Attr('foo')>>
final class Target {}

// Attr::class can't know the attribute's T.
function attribute_value(): int {
  $attr = (new ReflectionClass(Target::class))
    ->getAttributeClass(Attr::class);
  return $attr?->value ?? 0;
}

function attribute_value_mixed(): mixed {
  $attr = (new ReflectionClass(Target::class))
    ->getAttributeClass(Attr::class);
  return $attr?->value;
}

class Box<+T> {}

function takes_box_ptr(class<Box<mixed>> $_): void {}

function plain_class_ptrs(): void {
  takes_box_ptr(Box::class);
  $_ = Target::class;
}
