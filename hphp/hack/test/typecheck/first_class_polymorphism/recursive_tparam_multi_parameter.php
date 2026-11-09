<?hh
<<file:__EnableUnstableFeatures('polymorphic_function_hints')>>

class Cov<+T> {}
final class C extends Cov<C> {}

class A {
  public function two<T as Cov<TU>, TU as Cov<T>>(T $_, TU $_): void {}
  public function three<T as Cov<TU>, TU as Cov<TV>, TV as Cov<T>>(
    T $_,
    TU $_,
    TV $_,
  ): void {}
  public function dependent<T as Cov<T>, TU as Cov<T>>(T $_, TU $_): void {}
}

function pointer_types(): void {
  hh_expect<HH\FunctionRef<(readonly function<
    T as Cov<TU>,
    TU as Cov<T>,
  >(A, T, TU): void)>>(
    meth_caller(A::class, 'two'),
  );
  hh_expect<HH\FunctionRef<(readonly function<
    T as Cov<TU>,
    TU as Cov<TV>,
    TV as Cov<T>,
  >(A, T, TU, TV): void)>>(
    meth_caller(A::class, 'three'),
  );
  hh_expect<HH\FunctionRef<(readonly function<
    T as Cov<T>,
    TU as Cov<T>,
  >(A, T, Cov<T>): void)>>(
    meth_caller(A::class, 'dependent'),
  );
}

function valid(): void {
  $a = new A();
  $c = new C();
  $two = meth_caller(A::class, 'two');
  $two($a, $c, $c);
  $three = meth_caller(A::class, 'three');
  $three($a, $c, $c, $c);
  $dependent = meth_caller(A::class, 'dependent');
  $dependent($a, $c, new Cov<C>());
}
