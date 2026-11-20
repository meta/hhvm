<?hh

class C {
  <<__ShapeKey>>
  const classname<C> NAME = nameof C;
  <<__ShapeKey>>
  const int NUMBER = 1;
}

class Generic<T as arraykey> {
  <<__ShapeKey>>
  const int NUMBER = 1;
}

type ClassnameFields = shape(nameof C => int, C::NAME => string);

type ClassnameFirst = shape(nameof C => int, C::NUMBER => string);
type ClassnameSecond = shape(C::NUMBER => int, nameof C => string);
type GenericClassnameFirst = shape(
  nameof Generic => int,
  Generic::NUMBER => string,
);

function classname_fields(): void {
  $_ = shape(nameof C => 1, C::NAME => 'same type');
}

function mismatched_fields(): void {
  $_ = shape(nameof C => 1, C::NUMBER => 'wrong type');
  $_ = shape(C::NUMBER => 1, nameof C => 'wrong type');
  $_ = shape(Generic::NUMBER => 1, nameof Generic => 'wrong type');
}
