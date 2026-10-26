<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

<<file: __EnableUnstableFeatures('named_parameters')>>

function test(): void {
  $_ = (int ...$values, named string...) ==> {
    hh_expect_equivalent<vec<int>>($values);
  };
  $_ = (bool $flag, int ...$values, named string...) ==> {
    hh_expect_equivalent<vec<int>>($values);
    hh_expect_equivalent<bool>($flag);
  };
  $_ = (int ...$values, named string $label) ==> {
    hh_expect_equivalent<vec<int>>($values);
    hh_expect_equivalent<string>($label);
  };
  $_ = (named string $label, int $first, int ...$values) ==> {
    hh_expect_equivalent<string>($label);
    hh_expect_equivalent<int>($first);
    hh_expect_equivalent<vec<int>>($values);
  };
  $_ = (
    named string $label,
    int $first,
    int ...$values,
    named bool $flag,
    named float $weight,
    named string...
  ) ==> {
    hh_expect_equivalent<string>($label);
    hh_expect_equivalent<int>($first);
    hh_expect_equivalent<vec<int>>($values);
    hh_expect_equivalent<bool>($flag);
    hh_expect_equivalent<float>($weight);
  };
  $_ = (named string...) ==> {};
  $_ = (int $first, named bool $flag, named string...) ==> {
    hh_expect_equivalent<int>($first);
    hh_expect_equivalent<bool>($flag);
  };
  $_ = (named int..., named string $label) ==> {
    hh_expect_equivalent<string>($label);
  };
  $_ = (int $first, named bool..., named string $label) ==> {
    hh_expect_equivalent<int>($first);
    hh_expect_equivalent<string>($label);
  };
  $_ = (int ...$values, named float..., named string $label) ==> {
    hh_expect_equivalent<vec<int>>($values);
    hh_expect_equivalent<string>($label);
  };
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
    hh_expect_equivalent<vec<int>>($values);
    hh_expect_equivalent<string>($label);
  });
  take_extra_positional((...$values, named bool $flag) ==> {
    hh_expect_equivalent<vec<int>>($values);
    hh_expect_equivalent<bool>($flag);
  });
  take_extra_positional((named bool $flag, ...$values) ==> {
    hh_expect_equivalent<vec<int>>($values);
    hh_expect_equivalent<bool>($flag);
  });
  take_extra_positional((int $first, ...$rest, named bool $flag) ==> {
    hh_expect_equivalent<int>($first);
    hh_expect_equivalent<vec<int>>($rest);
    hh_expect_equivalent<bool>($flag);
  });
  take_fixed_positional((...$values, named bool $flag) ==> {
    hh_expect_equivalent<vec<int>>($values);
    hh_expect_equivalent<bool>($flag);
  });
}
//
//
