<?hh

namespace Loose;

trait TClosure {
  public static function looseClosure(): (function(): void) {
    return () ==> {};
  }
}
