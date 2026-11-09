<?hh
<<file:__EnableUnstableFeatures('polymorphic_function_hints')>>

class Cov<+T> {}
final class C extends Cov<C> {}

class A {
  public function f<T as Cov<T>>(T $_): void {}
  public function acyclic<T as arraykey>(T $_): void {}
}

function pointer_types(): void {
  hh_expect<HH\FunctionRef<(readonly function<T as Cov<T>>(A, T): void)>>(
    meth_caller(A::class, 'f'),
  );
  hh_expect<HH\FunctionRef<(readonly function<T as arraykey>(A, arraykey): void)>>(
    meth_caller(A::class, 'acyclic'),
  );
}

function valid(): void {
  $a = new A();
  $f = meth_caller(A::class, 'f');
  $f($a, new C());
  $acyclic = meth_caller(A::class, 'acyclic');
  $acyclic($a, 1);
  $acyclic($a, 'key');
}
