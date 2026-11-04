<?hh

class KeyPrefix {
  const string PREFIX = 'prefix_';
}

class SomeClass {}

class ClassKeys {
  <<__ShapeKey>>
  const string FORWARDED = KeyPrefix::PREFIX;
}

enum LiteralKeys: string as string {
  A = 'a';
  B = nameof SomeClass;
}

enum UnusedComputed: string as string {
  X = KeyPrefix::PREFIX;
}

<<__AllowUncheckedEnumValues>>
enum OptedOutLiteralKeys: string as string {
  Y = 'y';
}

type LiteralShape = shape(LiteralKeys::A => int, LiteralKeys::B => int);
type OptedOutLiteralShape = shape(OptedOutLiteralKeys::Y => int);
type ClassComputedShape = shape(ClassKeys::FORWARDED => int);

function accesses(LiteralShape $literal): void {
  $_ = shape(LiteralKeys::A => 1);
  $_ = Shapes::idx($literal, LiteralKeys::A);
}
