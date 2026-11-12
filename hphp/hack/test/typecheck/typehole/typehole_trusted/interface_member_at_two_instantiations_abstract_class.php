<?hh

<<__UNSAFE_AllowMultipleInstantiations>>
interface I<T> {
  public function foo(): T;
}

interface IString extends I<string> {}

interface IInt extends I<int> {}

abstract class ABoth implements IInt, IString {}

final class C extends ABoth {
  public function foo(): int {
    return 3;
  }
}

function expect_string(IString $x): string {
  return $x->foo();
}

<<__EntryPoint>>
function main(): void {
  expect_string(new C());
}
