<?hh

class C {
  public arraykey $i = 0;

  public function invalidate(): int {
    $this->i = "42";
    return 0;
  }
}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main1(): void {
  $c = new C();
  $v = Vector<int> {0};
  if ($c->i is int) {
    // Soundness error call to invalidate must invalidate `$c->i : int`.
    $v[$c->invalidate()] = $c->i;
    // HHVM fatals
    takes_int($v[0]);
  }
}
