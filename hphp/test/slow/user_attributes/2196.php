<?hh

final class B {
  final public function foo() :mixed{ echo __METHOD__, "\n"; }
  final public static function bar() :mixed{ echo __METHOD__, "\n"; }

  <<__NEVER_INLINE>>
  public function callFoo(): void {
    $this->foo();
  }
}

abstract final class AbsFinalClass {
  public static function f() :mixed{ echo __METHOD__, "\n"; }
}

<<__MockClass>>
class C extends B {
  public function foo() :mixed{ echo __METHOD__, "\n"; }
  public static function bar() :mixed{ echo __METHOD__, "\n"; }
}

<<__MockClass>>
abstract final class MockAbsFinalClass extends AbsFinalClass {
  public static function f() :mixed{ echo __METHOD__, "\n"; }
}

class FinalMethodBase {
  final public function foo(): void {
    echo __METHOD__, "\n";
  }

  <<__NEVER_INLINE>>
  public function callFoo(): void {
    $this->foo();
  }
}

<<__MockClass>>
class MockFinalMethod extends FinalMethodBase {
  public function foo(): void {
    echo __METHOD__, "\n";
  }
}

function test() :mixed{
  $obj = new C;
  $obj->foo();
  C::bar();

  MockAbsFinalClass::f();
  (new B())->callFoo();
  $obj->callFoo();
  (new FinalMethodBase())->callFoo();
  (new MockFinalMethod())->callFoo();
}

<<__EntryPoint>>
function main_2196() :mixed{
test();
}
