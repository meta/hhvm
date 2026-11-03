<?hh

class C<T> {
  public function __construct(private shape(T...) $s) {}
  public function get(): shape(T...) {
    return $this->s;
  }
}

class D<T> extends C<int> {
  public function __construct() {
    parent::__construct(shape('k' => 1));
  }
}

trait Tr<T> {
  public function idx(shape(T...) $s): ?T {
    return Shapes::idx($s, 'k');
  }
}

class U<T> {
  use Tr<int>;
}

class Named<T> extends NamedBase<int> {}

class NamedBase<T> {
  public function get(shape(?'k' => T) $s): ?T {
    return Shapes::idx($s, 'k');
  }
}

function take_string(?string $_): void {}

function inherited_tail(D<string> $d): void {
  $s = $d->get();
  if (Shapes::keyExists($s, 'k')) {
    take_string($s['k']);
  }
}

function trait_tail(U<string> $u): ?int {
  return $u->idx(shape('k' => 'not an int'));
}

function named_field_control(Named<string> $n): void {
  take_string($n->get(shape('k' => 1)));
}
