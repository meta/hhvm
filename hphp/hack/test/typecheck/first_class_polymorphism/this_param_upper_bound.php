<?hh

interface Consumer<-T> {
  public function accept(T $value): void;
}

final class Wrapper<T> {}

abstract class A {
  public function check<T as this>(T $other): void {
    $this->guard($other);
  }

  public function indirect<TArg as TBase, TBase as this>(TArg $other): void {
    $this->guard($other);
  }

  public function where_bound<TArg>(TArg $other): void where TArg as this {
    $this->guard($other);
  }

  public function consume<TArg as Consumer<this>>(TArg $_): void {}

  public function recursive<TArg as Wrapper<TArg>>(TArg $_): void {}

  private function guard(this $_): void {}
}

final class X extends A {}
final class Y extends A {}
final class F extends A {}

function invalid(): void {
  $f = meth_caller(A::class, 'check');
  $f(new X(), new Y());
  $indirect = meth_caller(A::class, 'indirect');
  $where = meth_caller(A::class, 'where_bound');
}

function valid(): void {
  $consume = meth_caller(A::class, 'consume');
  $recursive = meth_caller(A::class, 'recursive');
  $f = meth_caller(F::class, 'check');
  $f(new F(), new F());
}
