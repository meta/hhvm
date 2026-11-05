<?hh

<<file:__EnableUnstableFeatures('union_intersection_type_hints')>>

interface HasU {
  abstract const type U;
  abstract const type T;
}

abstract class A implements HasU {
  const type T = this::U;
}

abstract class B implements HasU {
  const type T = this::U;
}

abstract class Holder {
  abstract const type C as ((A | B) & HasU with { type U = int });
  abstract public function get(): this::C;

  public function check(this::C $c): void {
    needs_t_int($c);
  }
}

function needs_t_int(HasU with { type T = int } $_): void {}

function check(Holder $h): void {
  $c = $h->get();
  needs_t_int($c);
}
