<?hh

class C<Tm> {
  public function __construct(private class<Tm> $className) {
    $class = $this->className;
    new $class();
  }
}

class D {
  public function __construct(public int $i) {
    echo $i;
  }
}

<<__EntryPoint>>
function main(): void {
  new C(D::class);
}
