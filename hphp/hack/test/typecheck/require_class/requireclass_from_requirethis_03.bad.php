<?hh

<<file:__EnableUnstableFeatures('require_constraint')>>

class C {
  public function bar(): void { echo "bar\n"; }
}

class D {
  use T;
}

trait T {
  /* HH_IGNORE[12043] require-class constraint; class does not use trait */
  require class C;

  public function foo(): void {
    $this->bar();
  }
}
