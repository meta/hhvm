//// newtype.php
<?hh

newtype bar = int;
class C1 {
  const bar FOO = 0;
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

// Reject: type of keys is opaque
type myshape = shape(C2::FOO => int);
