<?hh

<<file: __EnableUnstableFeatures('class_const_default')>>

interface IHasDefault {
  abstract const int FROM_INTERFACE = 1;
}

abstract class Base implements IHasDefault {
  abstract const string KIND = 'unknown';
  abstract const int NO_DEFAULT;

  public static function kind(): string {
    return static::KIND; // ok: the default resolves on any subclass
  }

  public static function fromInterface(): int {
    return static::FROM_INTERFACE; // ok
  }

  public static function noDefault(): int {
    return static::NO_DEFAULT; // error
  }
}

abstract class Overrides extends Base {
  const string KIND = 'a';
}

abstract class NoOverride extends Base {}

trait TRequiresHasDefault {
  require implements IHasDefault;

  public static function fromTrait(): int {
    return static::FROM_INTERFACE; // ok
  }
}

abstract final class Caller {
  public static function call(): void {
    Overrides::kind(); // ok: kind() does not need a concrete class
    NoOverride::kind(); // ok
    NoOverride::fromInterface(); // ok
  }
}
