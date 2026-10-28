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

  public static function type_structure_ok(): void {
    // Covered by needs_concrete_abstract_type_const_check, which is off here
    type_structure(static::class, 'T');
  }
}
