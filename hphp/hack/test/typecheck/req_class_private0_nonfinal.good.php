<?hh

class DemoClass {
  use TDemo;
  private static function forPrivate(): void {
    echo "private";
  }

  public static function bleh(): void {
    DemoClass::forPrivate();
  }

  public static function blah(): void {
    self::forPrivate();
  }
}

trait TDemo {
  require class DemoClass;

  public static function demoFunc(): void {
    DemoClass::forPrivate();
  }
}

<<__EntryPoint>>
function main(): void {
  DemoClass::demoFunc();
}
