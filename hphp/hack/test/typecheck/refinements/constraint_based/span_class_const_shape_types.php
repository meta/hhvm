<?hh

final class HasConst {
  <<__ShapeKey>>
  const string A = 'a';
}

function f(shape(HasConst::A => int) $x): void {
  if ($x is shape(HasConst::A => int)) {
    hh_expect<shape(HasConst::A => int)>($x);
  } else {
    // should not split to nothing
    hh_show($x);
  }
}

function g(shape(HasConst::A => int) $x): void {
  if ($x is shape('a' => int)) {
    hh_expect<shape('a' => int)>($x);
  } else {
    // should not split to nothing
    hh_show($x);
  }
}

function h(shape('a' => int) $x): void {
  if ($x is shape(HasConst::A => int)) {
    hh_expect<shape(HasConst::A => int)>($x);
  } else {
    // should not split to nothing
    hh_show($x);
  }
}
