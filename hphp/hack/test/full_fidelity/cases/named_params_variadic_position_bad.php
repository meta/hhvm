<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.
<<file: __EnableUnstableFeatures('named_parameters')>>

function before_named(named int..., named string $fixed): void {}
function before_positional(named int..., int $fixed): void {}
function before_rest(named int..., string ...$xs): void {}
function positional_rest(int ...$xs, named bool $flag, int $p): void {}

abstract class Example {
  public function __construct(named int..., int $p) {}
  abstract public function m(named int..., named int $fixed): void;
}

interface I {
  public function j(named int..., named int $fixed): void;
}

function lambdas(): void {
  $_ = (named int..., named int $fixed) ==> $fixed;
  $_ = function(named int..., named int $fixed): void {};
}

type BeforeNamed = (function(named int..., named string $fixed): void);
type BeforePositional = (function(named int..., int): void);
type BeforeRest = (function(named int..., string...): void);
type PositionalRest = (function(int..., named bool $flag, int): void);
type Nested = (function((function(named int..., int): void)): void);
function ro(named string..., named readonly string $s): void {}
function duplicate(named int..., named string...): void {}
//
//
