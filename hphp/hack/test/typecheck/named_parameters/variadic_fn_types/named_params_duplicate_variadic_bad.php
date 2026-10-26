<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.
<<file: __EnableUnstableFeatures('named_parameters')>>

function two(named int..., named string...): void {}
function two_after_positional(int ...$rest, named int..., named string...): void {}
function three(named int..., named string..., named bool...): void {}

abstract class C {
  abstract public function m(named bool..., named int...): void;
}

interface I {
  public function j(named mixed..., named int...): void;
}

function in_lambdas(): void {
  $_ = (named int..., named string...) ==> 1;
  $_ = function(named int..., named string...): void {};
}

function in_fn_type((function(named int..., named string...): void) $_): void {}
//
//
