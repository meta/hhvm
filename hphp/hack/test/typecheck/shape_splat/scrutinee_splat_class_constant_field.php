<?hh
<<file:__EnableUnstableFeatures('union_intersection_type_hints')>>

final class FieldNames {
  <<__ShapeKey>>
  const string A = 'a';
}

function f(shape(FieldNames::A => int) $x): void {
  if ($x is shape(FieldNames::A => string)) {
    hh_expect_equivalent<(
      shape(FieldNames::A => int) & shape(FieldNames::A => string)
    )>($x);
  }
}
