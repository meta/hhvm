<?hh

// A class<Break13Abstract> pointer can name the abstract class itself, so the
// static call hits an abstract method; __NeedsConcrete only covers static::.
class Break13 {
  public static function run(): int {
    return self::make(Break13Abstract::class);
  }

  private static function make(class<Break13Abstract> $cls): int {
    return $cls::make();
  }
}

abstract class Break13Abstract {
  abstract public static function make(): int;
}
