<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

<<file: __EnableUnstableFeatures('named_parameters')>>

function expect_string(string $_): void {}

function expect_int(int $_): void {}

function expect_bool(bool $_): void {}

function test(): void {
  $_ = (int ...$values, named string...) ==> {
    expect_string($values[0]); // Error
  };
  $_ = (int ...$values, named string $label) ==> {
    expect_string($values[0]); // Error
    expect_int($label); // Error
  };
  $_ = (named string $label, int ...$values, named bool $flag, named float...) ==> {
    expect_int($label); // Error
    expect_string($values[0]); // Error
    expect_int($flag); // Error
  };
  // TODO(T289877278): Restore these as diagnostic-recovery tests for rejected
  // non-final named variadic parameters (D123578097).
  // $_ = (named int..., named string $label) ==> {
  //   expect_int($label); // Error
  // };
  // $_ = (int $first, named bool..., named string $label) ==> {
  //   expect_string($first); // Error
  //   expect_int($label); // Error
  // };
  // $_ = (int ...$values, named float..., named string $label) ==> {
  //   expect_string($values[0]); // Error
  //   expect_int($label); // Error
  // };
}

function take_variadic(
  (function(int..., named string $label, named bool...): void) $_,
): void {}

function take_extra_positional(
  (function(int, int, int..., named bool $flag): void) $_,
): void {}

function take_fixed_positional(
  (function(int, int, int, named bool $flag): void) $_,
): void {}

function test_inferred(): void {
  take_variadic((...$values, named $label, named bool...) ==> {
    expect_string($values[0]); // Error
    expect_int($label); // Error
  });
  take_extra_positional((...$values, named bool $flag) ==> {
    expect_bool($values[0]); // Error
    expect_int($flag); // Error
  });
  take_extra_positional((named bool $flag, ...$values) ==> {
    expect_bool($values[0]); // Error
    expect_int($flag); // Error
  });
  take_extra_positional((int $first, ...$rest, named bool $flag) ==> {
    expect_string($first); // Error
    expect_bool($rest[0]); // Error
    expect_int($flag); // Error
  });
  take_fixed_positional((...$values, named bool $flag) ==> {
    expect_bool($values[0]); // Error
    expect_int($flag); // Error
  });
}
