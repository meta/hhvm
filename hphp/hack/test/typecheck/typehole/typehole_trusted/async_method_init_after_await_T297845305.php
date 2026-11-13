<?hh

final class C {
  private int $n;
  private ?Awaitable<void> $pending;

  public function __construct() {
    $this->pending = $this->init();
    $this->read();
  }

  private async function init(): Awaitable<void> {
    await HH\Asio\later();
    $this->n = 1;
  }

  public function read(): void {
    self::expectInt($this->n);
  }

  private static function expectInt(int $_): void {}
}

<<__EntryPoint>>
function main(): void {
  new C();
}
