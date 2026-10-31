//// use.php
<?hh

newtype blah = shape(
  Foo::BAR => int,
);

//// def.php
<?hh

newtype blah2 = string;
class Foo {
  <<__ShapeKey>>
  const blah2 BAR = 'bar';
}
