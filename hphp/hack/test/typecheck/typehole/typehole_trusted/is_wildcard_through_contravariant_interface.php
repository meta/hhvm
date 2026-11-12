<?hh

interface IContra<-T> {}

interface IContraChild<T> extends IContra<T> {
  public function set(T $x): void;
}

function test(IContra<arraykey> $x): void {
  if ($x is IContraChild<_>) {
    $x->set('not an int');
  }
}

class C implements IContra<arraykey>, IContraChild<int> {
  public function set(int $x): void {}
}

<<__EntryPoint>>
function main(): void {
  test(new C());
}
