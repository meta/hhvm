//// newtype.php
<?hh

newtype bar = bool;
class C1 {
  const bar FOO = true;
}

//// def.php
<?hh

newtype foo = bar;
class C2 {
  <<__ShapeKey>>
  const foo FOO = C1::FOO;
}

//// use.php
<?hh

// Should fail because C2::FOO is really a bool
type myshape = shape(C2::FOO => int);
