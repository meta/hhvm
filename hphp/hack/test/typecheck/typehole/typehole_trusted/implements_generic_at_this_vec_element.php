<?hh

interface Sink<T> {
  public function put(T $x): void;
}

class C implements Sink<this> {
  public function put(this $x): void {}
}

class D extends C {
  public function onlyD(): int { return 1; }
  <<__Override>>
  public function put(this $x): void { $x->onlyD(); }
}

function put_c(Sink<C> $s): void {
  $s->put(new C());
}

function put_first(vec<C> $v): void {
  put_c($v[0]);
}

<<__EntryPoint>>
function main(): void {
  put_first(vec[new D()]);
}
