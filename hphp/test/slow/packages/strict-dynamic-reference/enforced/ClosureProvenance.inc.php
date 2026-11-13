<?hh

namespace StrictEnforced;

function closure(): (function(): void) {
  return () ==> {};
}

function convert(string $class): void {
  \HH\classname_to_class($class);
}

final class LooseTraitUser {
  use \Loose\TClosure;
}
