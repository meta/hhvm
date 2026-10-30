<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

<<file: __EnableUnstableFeatures('named_parameters')>>

function f(named string $label, int $first, int ...$rest): void {}

function test(): void {
  f("wrong", label = "ok");
  f(1, label = 123);
}
//
