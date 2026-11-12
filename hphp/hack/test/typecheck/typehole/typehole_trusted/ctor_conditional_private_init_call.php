<?hh
final class C {
  private int $x;
  public function __construct(bool $c) {
    $c ? $this->initX() : null;
    takes_int($this->x);
  }
  private function initX(): void { $this->x = 1; }
}
function takes_int(int $x): void { var_dump($x); }
<<__EntryPoint>> function main(): void { new C(false); }
