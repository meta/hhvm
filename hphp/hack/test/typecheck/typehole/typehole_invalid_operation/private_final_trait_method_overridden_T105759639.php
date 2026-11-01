<?hh

trait MyTrait {
  public function foo(): void { $this->bar(); }
  final private function bar(): void {}
}

class MyClass {
  use MyTrait;
}

class MyChild extends MyClass {
  private function bar(): void {}
}
