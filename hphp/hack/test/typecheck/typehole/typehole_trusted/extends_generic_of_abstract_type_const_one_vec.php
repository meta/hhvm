<?hh

class Box<T> {
  public function __construct(private T $data) {}
  public function get(): T { return $this->data; }
  public function set(T $x): void { $this->data = $x; }
}

abstract class C extends Box<this::T> {
  abstract const type T as arraykey;
}
class CS extends C { const type T = string; }
class CI extends C { const type T = int; }

function transfer<T>(vec<Box<T>> $v): void {
  $v[0]->set($v[1]->get());
}

function transfer_all(vec<C> $v): void {
  transfer($v);
}

<<__EntryPoint>>
function main(): string {
  $cs = new CS("hi");
  transfer_all(vec[$cs, new CI(42)]);
  return $cs->get();
}
