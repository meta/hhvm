<?hh

namespace StrictNotice;

function convert(string $class): void {
  \HH\classname_to_class($class);
}

final class EnforcedTraitUser {
  use \StrictEnforced\TClosure;
}
