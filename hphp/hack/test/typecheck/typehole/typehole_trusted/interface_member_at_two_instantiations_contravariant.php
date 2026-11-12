<?hh

class A {}

class B {}

<<__UNSAFE_AllowMultipleInstantiations>>
interface IObserve<-T> {
  public function observe(T $x): void;
}

interface IObserveA extends IObserve<A> {}

interface IObserveB extends IObserve<B> {}

interface IObserveBoth extends IObserveA, IObserveB {}

final class ObservesA implements IObserveBoth {
  public function observe(A $x): void {}
}

function observe_b(IObserveB $o): void {
  $o->observe(new B());
}

<<__EntryPoint>>
function main(): void {
  observe_b(new ObservesA());
}
