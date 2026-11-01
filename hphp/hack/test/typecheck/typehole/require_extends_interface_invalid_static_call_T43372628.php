<?hh

abstract class C1 {
  final public static function do(): int {
    return 42;
  }
}

interface I1 {
  require extends C1;
}

abstract final class A1 {
  final public static function do(): int {
    $i1 = I1::class;
    return $i1::do();
  }
}

<<__EntryPoint>>
function main():void {
  A1::do();
}
