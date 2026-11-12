<?hh

<<__ConsistentConstruct>>
abstract class Cov<+T> {
  public function __construct(private T $x) {}

  public function get(): T {
    return $this->x;
  }

  <<__NeedsConcrete>>
  public static function make(): this {
    $c = static::class;
    return new $c(1.5);
  }
}

final class IntCov extends Cov<int> {}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  takes_int(IntCov::make()->get());
}
