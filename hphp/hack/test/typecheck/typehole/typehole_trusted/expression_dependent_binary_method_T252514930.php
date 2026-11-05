<?hh

class AAA {
  public function binop(this $_other): bool {
    return false;
  }
}

class BBB extends AAA {
  private function onlyInBBB(): void {}
  public function binop(this $other): bool {
    $other->onlyInBBB();
    return true;
  }
}

interface IWithTyConst {
  abstract const type T as AAA;
  public function blip(): this::T;
  public function bloop(): this::T;
}

class ImplementsIt implements IWithTyConst {
  const type T = AAA;

  public function blip(): this::T {
    return new AAA();
  }

  public function bloop(): this::T {
    return new BBB();
  }
}

function test(IWithTyConst $it): void {
  $x = $it
    ->blip(); // ~<expr#2>::T where <expr#2>::T as IWithTyConst::T but really AAA when called with ImplementsIt
  $y = $it
    ->bloop(); // ~<expr#2>::T where <expr#2>::T as IWithTyConst::T but really BBB when called with ImplementsIt

  // hh thinks this is fine since the 'expression dependent' types are equal... it is not and will fatal
  $result = $y->binop($x);
}

<<__EntryPoint>>
function entry(): void {
  test(new ImplementsIt());
}
