<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

// TODO(T294006289): replace with positive coverage when mixed definitions are supported.
function mixed_function(inout int $x, named string $s): void {}

class MixedClass {
  public function mixed_method(named string $s, inout int $x): void {}
}

interface MixedInterface {
  public function mixed_method(inout int $x, named string $s): void;
}

function mixed_closures(): void {
  $_ = function(inout int $x, named string $s): void {};
  $_ = (named string $s, inout int $x) ==> {};
}

type MixedFunctionType = (function(inout int, named string $s): void);

// Only the existing incompatible-modifier error should be reported for overlaps.
function overlap_first(inout named int $x, named int $y): void {}
function overlap_last(inout int $x, named int $y, inout named int $z): void {}
type OverlappingFunctionType = (function(inout int, inout named int $x): void);
