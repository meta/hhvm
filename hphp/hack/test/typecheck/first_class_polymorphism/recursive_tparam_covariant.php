<?hh
<<file:__EnableUnstableFeatures('polymorphic_function_hints')>>

interface I {}
class Cov<+T> implements I {}

class A {
  public function f<T super Cov<T>>(): T {
    return new Cov<T>();
  }
}

function pointer_types(): void {
  $f = meth_caller(A::class, 'f');
  hh_show($f);
}

function valid(): I {
  $a = new A();
  return $a->f<I>();
}
