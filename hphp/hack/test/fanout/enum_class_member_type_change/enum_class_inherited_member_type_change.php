//// base-builtins.php
<?hh
// The light fanout harness does not load HHI declarations.
namespace HH {
  newtype MemberOf<-TEnumClass, +TType> as TType = TType;
  abstract class BuiltinAbstractEnumClass {}
  abstract class BuiltinEnumClass<+T> {}
}
namespace HH\EnumClass {
  newtype Label<-TEnumClass, +TType> = mixed;
}

//// base-E.php
<?hh
abstract enum class E: mixed {
  int A = 1;
  abstract bool B;
}

//// base-F.php
<?hh
abstract enum class F: mixed extends E {}

//// base-use.php
<?hh
/**
 * Changing inherited B's type and abstractness must recheck this label switch.
 * Making B a concrete int adds a required case even though F does not change.
 */
<<__StrictSwitch>>
function use_f(\HH\EnumClass\Label<F, int> $x): void {
  switch ($x) {
    case F#A:
      return;
  }
}

//// changed-E.php
<?hh
abstract enum class E: mixed {
  int A = 1;
  int B = 2;
}
