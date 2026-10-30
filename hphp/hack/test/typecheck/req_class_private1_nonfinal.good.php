<?hh

class DemoClass {
  use TDemo;
  private static function forPrivate(): void {
    echo "private";
  }
}

trait TDemo {
  require class DemoClass;

  public static function demoFunc(): void {
    self::forPrivate();
  }
}

<<__EntryPoint>>
function main(): void {
  DemoClass::demoFunc();
}
