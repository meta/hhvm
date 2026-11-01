<?hh

// HHVM rejects a static method that redeclares a private instance method the
// parent got from a trait.
trait THgoldstein {
  private function foo(): void {}
}

abstract class HgoldsteinParent {
  use THgoldstein;
}

final class HgoldsteinChild extends HgoldsteinParent {
  public static function foo(): int {
    return 42;
  }
}

<<__EntryPoint>>
function main(): void {
  new HgoldsteinChild();
}
