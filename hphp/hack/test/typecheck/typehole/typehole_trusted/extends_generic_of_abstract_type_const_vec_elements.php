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

function transfer<T>(Box<T> $to, Box<T> $from): void {
  $to->set($from->get());
}

function transfer_first(vec<C> $v): void {
  transfer($v[0], $v[1]);
}

<<__EntryPoint>>
function main(): string {
  $cs = new CS("hi");
  transfer_first(vec[$cs, new CI(42)]);
  return $cs->get();
}
