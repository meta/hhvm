<?hh

class A<+T1, -T2 as T1> {
  private ?T2 $contents;
  public function __construct() {}
  public function put(T2 $x): void { $this->contents = $x; }
  public function get(): ?T1 {
    return $this->contents;
  }
}

function test(): void {
  $x = new A(); // x : A<v1, v2>, -v2 <: +v1
  $x->put(0); // int <: -v2
  take_string_opt($x->get()); // +v1 <: string -> should error
}

function take_string_opt(?string $x): void {
  var_dump($x);
}
