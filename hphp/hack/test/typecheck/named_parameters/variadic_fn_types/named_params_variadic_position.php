<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.
<<file: __EnableUnstableFeatures('named_parameters', 'union_intersection_type_hints')>>

function after_named(int $p, named string $fixed, named int...): void {}
function after_rest(int ...$xs, named string $fixed, named bool...): void {}
function after_pos(int $p, named int...): void {}

function calls(): void {
  after_named(1, fixed = 'ok', extra = 2, more = 3);
  after_named(1, fixed = 'ok');
  after_rest(1, 2, 3, fixed = 'ok', flag = true);
  after_rest(fixed = 'ok');
  after_pos(1, extra = 2);
  $child = new Child(1, extra = 2);
  $child->m(1, fixed = 'ok', extra = 2);
}

abstract class Base {
  abstract public function m(int $p, named string $fixed, named int...): void;
}

final class Child extends Base {
  public function __construct(int $p, named int...) {}
  public function m(int $p, named string $fixed, named int...): void {}
}

interface HasFinal {
  public function j(named string $fixed, named mixed...): void;
}

function lambdas(): void {
  $short = (int $p, named string $fixed, named int...) ==> $p;
  $long = function(int $p, named string $fixed, named int...): void {};
  $short(1, fixed = 'ok', extra = 2);
  $long(1, fixed = 'ok', extra = 2);
}

type FinalHint = (function(int, named string $fixed, named int...): void);
type Nested = (function((function(int, named string $s, named int...): void)): void);

function take(FinalHint $f): void {
  $f(1, fixed = 'ok', extra = 3);
}

function pass(): void {
  take(after_named<>);
}

function ro(named readonly string $fixed, named string...): void {}
function call_ro(readonly string $s): void {
  ro(fixed = $s, extra = 'ok');
}

function contextual((function(int, string, named bool $flag): void) $f): void {}

function aligned_capture(): void {
  contextual((...$args, named $flag) ==> {
    hh_expect_equivalent<vec<(int | string)>>($args);
    hh_expect_equivalent<bool>($flag);
  });
  contextual((...$args, named bool $flag) ==> {
    hh_expect_equivalent<vec<(int | string)>>($args);
    hh_expect_equivalent<bool>($flag);
  });
}
//
//
