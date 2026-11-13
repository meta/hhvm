<?hh

namespace StrictEnforced;

trait TClosure {
  public static function closure(): (function(): void) {
    return () ==> {};
  }
}
