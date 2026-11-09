<?hh
<<file:__EnableUnstableFeatures('polymorphic_function_hints')>>

class Cov<+T> {}
class Inv<T> {}
final class C extends Cov<C> {}
final class D extends Inv<D> {}

class A {
  public function f<T as Cov<T>>(T $value): T {
    return $value;
  }
  public function bound<T as Inv<T>>(T $_): void {}
}

function pointer_types(): void {
  hh_expect<HH\FunctionRef<(readonly function<T as Cov<T>>(A, T): T)>>(
    meth_caller(A::class, 'f'),
  );
  hh_expect<HH\FunctionRef<(readonly function<T as Inv<T>>(A, T): void)>>(
    meth_caller(A::class, 'bound'),
  );
}

function valid(): C {
  $a = new A();
  $bound = meth_caller(A::class, 'bound');
  $bound($a, new D());
  $f = meth_caller(A::class, 'f');
  return $f($a, new C());
}
