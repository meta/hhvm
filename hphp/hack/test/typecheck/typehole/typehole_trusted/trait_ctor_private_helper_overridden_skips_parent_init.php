<?hh

class P {
  public function __construct(public int $x): void {}
}

trait T {
  require extends P;
  public function __construct(): void {
    $this->stop();
  }
  private function stop(): void {
    throw new Exception('no instance');
  }
}

class C extends P {
  use T;
  protected function stop(): void {}
}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  takes_int((new C())->x);
}
