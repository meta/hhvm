<?hh

interface I {}
class A implements I {}
class B implements I {}

function test(): I {
  $x = dict['a' => 1];
  if (false) {
    $y = new A();
  } else {
    $y = new B();
  }
  $z = $x[$y];
  return $y;
}
