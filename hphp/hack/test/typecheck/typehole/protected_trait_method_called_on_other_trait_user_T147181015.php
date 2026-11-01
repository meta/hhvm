<?hh

// Inside the trait, Class1's protected foo() looks accessible, but from
// Class2's copy of bar() it is not.
trait TraitWithProtectedMethod {
  protected function foo(): void {}
  public function bar(): void {
    (new Class1())->foo();
  }
}
final class Class1 {
  use TraitWithProtectedMethod;
}
final class Class2 {
  use TraitWithProtectedMethod;
}

<<__EntryPoint>>
function main(): void {
  (new Class2())->bar();
}
