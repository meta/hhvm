<?hh

interface ICov<+T> {}

interface ICovChild<T> extends ICov<T> {
  public function get(): T;
}

function expect_int(int $_): void {}

function test(ICov<int> $x): void {
  if ($x is ICovChild<_>) {
    expect_int($x->get());
  }
}

class C implements ICov<int>, ICovChild<arraykey> {
  public function get(): arraykey {
    return 'foo';
  }
}

<<__EntryPoint>>
function main(): void {
  test(new C());
}
