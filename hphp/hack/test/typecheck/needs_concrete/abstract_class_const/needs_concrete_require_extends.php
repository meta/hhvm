<?hh

abstract class Base {
  abstract const int X;
  abstract const type T;
}

trait TRequireExtends {
  require extends Base;

  public static function error(): void {
    static::X; // error
  }

  public static function type_structure_error(): void {
    // error, reported once: by the type constant check, not the class constant one
    type_structure(static::class, 'T');
  }
}
