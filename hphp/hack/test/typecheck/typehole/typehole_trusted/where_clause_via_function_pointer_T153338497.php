<?hh

abstract class MyBox {
  abstract const type TInner;
  abstract public function set(this::TInner $_): void;
}

class IntBox extends MyBox {
  const type TInner = int;
  private int $v = 0;
  public function set(int $x): void { $this->v = $x; }
  public function get(): int { return $this->v; }
}

  function set_a_box<TBox as nonnull as MyBox, TVal>(TBox $b, TVal $v): void where TVal = TBox::TInner {
  $b->set($v);
}

<<__EntryPoint>>
function test(): void {
  $fun = set_a_box<>;
  $x = new IntBox();
  $fun($x, "hello");
  $x->get();
}
