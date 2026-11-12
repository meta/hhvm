<?hh
function expose(A $a): bool { takes_int($a->get()); return true; }
function takes_int(int $x): void { var_dump($x); }
class A {
 public function __construct(bool $run) { $run && expose($this); }
 public function get(): int { return 0; }
}
final class B extends A {
 private int $x;
 public function __construct() { parent::__construct(true); $this->x = 123; }
 public function get(): int { return $this->x; }
}
<<__EntryPoint>> function main(): void { new B(); }
