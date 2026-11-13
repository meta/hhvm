<?hh

// Closure classes are never in a strict package, wherever they are defined.
// In repo mode HHBBC also clones a trait's closures into each class using the
// trait, which must not change that.

function closure_attempt(string $label, (function(): void) $operation): void {
  try {
    $operation();
    echo $label.": allowed\n";
  } catch (InvalidOperationException $_) {
    echo $label.": denied\n";
  }
}

<<__EntryPoint>>
function closure_provenance_main(): void {
  require_once __DIR__."/enforced/Targets.inc.php";
  require_once __DIR__."/enforced/TClosure.inc.php";
  require_once __DIR__."/loose/TClosure.inc.php";
  require_once __DIR__."/enforced/ClosureProvenance.inc.php";
  require_once __DIR__."/notice/ClosureProvenance.inc.php";
  require_once __DIR__."/loose/ClosureProvenance.inc.php";

  set_error_handler((int $_errno, string $_message) ==> {
    echo "notice\n";
    return true;
  });

  $target = __hhvm_intrinsics\launder_value(nameof StrictEnforced\Target)."";
  $enforced = get_class(StrictEnforced\closure());
  $in_loose = get_class(Loose\EnforcedTraitUser::closure());
  $in_notice = get_class(StrictNotice\EnforcedTraitUser::closure());
  $loose = get_class(StrictEnforced\LooseTraitUser::looseClosure());

  closure_attempt(
    "enforced-class:outside",
    () ==> HH\classname_to_class($target),
  );
  closure_attempt(
    "enforced-closure:outside",
    () ==> HH\classname_to_class($enforced),
  );
  closure_attempt(
    "enforced-closure-in-loose-class:enforced",
    () ==> StrictEnforced\convert($in_loose),
  );
  closure_attempt(
    "enforced-closure-in-loose-class:outside",
    () ==> HH\classname_to_class($in_loose),
  );
  closure_attempt(
    "enforced-closure-in-notice-class:enforced",
    () ==> StrictEnforced\convert($in_notice),
  );
  closure_attempt(
    "enforced-closure-in-notice-class:notice",
    () ==> StrictNotice\convert($in_notice),
  );
  closure_attempt(
    "loose-closure-in-enforced-class:outside",
    () ==> HH\classname_to_class($loose),
  );
}
