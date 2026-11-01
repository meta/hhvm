<?hh

// A memoized parameter of generic type is accepted whatever it's instantiated
// to, so BBBB passes a TTTT, which HHVM can't use as a memoization key.
final class BBBB extends TTTT<TTTT<int>> {
  public static function xx(): void {
    $t = new BBBB();
    $t->build(new TTTT<int>());
  }
}

class TTTT<TTT> {
  <<__Memoize>>
  public function build(TTT $t): int {
    return 1;
  }
}

<<__EntryPoint>>
function main(): void {
  BBBB::xx();
}
