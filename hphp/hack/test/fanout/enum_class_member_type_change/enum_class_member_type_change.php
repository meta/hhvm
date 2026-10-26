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
enum class E: mixed {
  int A = 1;
  bool B = true;
}

//// base-use.php
<?hh
/**
 * Changing B from bool to int must recheck this previously exhaustive switch.
 * The bound initially excludes B, so the switch never references it directly.
 */
<<__StrictSwitch>>
function use_e(\HH\MemberOf<E, int> $x): void {
  switch ($x) {
    case E::A:
      return;
  }
}

//// changed-E.php
<?hh
enum class E: mixed {
  int A = 1;
  int B = 2;
}
