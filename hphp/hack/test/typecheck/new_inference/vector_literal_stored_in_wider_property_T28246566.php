<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

class C {
  public function __construct(
    private Vector<arraykey> $y,
  ) {}

  public function test(bool $c): Vector<int> {
    $x = Vector {42};
    $this->y = $c ? $x : Vector {'foo'};
    return $x;
  }

  public function blow(): void {
    $x = $this->test(true);
    $this->y[0] = 'bar';
    $this->f($x[0]);
  }

    private function f(int $_): void {}
}
