<?hh

class MyFoo {
  use MyTrait;

  private function bong(): void {}

  private static function staticBong(): void {}
}

trait MyTrait {
  require class MyFoo;
  public function bang(): void {
    $this->bong();      // OK
    self::staticBong(); // OK
  }
}
