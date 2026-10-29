<?hh
<<file: __EnableUnstableFeatures('named_parameters', 'union_intersection_type_hints')>>

function take_named_fn((function(named int $a, named string $b): void) $_): void {}
function wants_int(int $x): void {}
function wants_string(string $x): void {}

function reordered_lambda_accepts(): void {
  take_named_fn((named $b, named $a) ==> {
    wants_int($a);
    wants_string($b);
  });
}

function optional_extra_named_accepts(): void {
  take_optional_extra_fn((named $extra = "s", $x, named $name) ==> {
    wants_string($extra);
    wants_int($x);
    wants_string($name);
  });
}

function take_optional_extra_fn(
  (function(int, named string $name): void) $_,
): void {}

function optional_extra_positional_accepts(): void {
  take_optional_extra_positional_fn(($extra = "s", named $name) ==> {
    wants_string($extra);
    wants_int($name);
  });
}

function take_optional_extra_positional_fn(
  (function(named int $name): void) $_,
): void {}

function same_order_lambda_accepts(): void {
  take_named_fn((named $a, named $b) ==> {
    wants_int($a);
    wants_string($b);
  });
}

function annotated_reordered_lambda_accepts(): void {
  take_named_fn((named string $b, named int $a) ==> {
    wants_int($a);
    wants_string($b);
  });
}

function mixed_positional_and_reordered_named_accepts(): void {
  take_mixed_fn(($x, named $b, named $a) ==> {
    wants_int($x);
    wants_int($a);
    wants_string($b);
  });
}

function take_mixed_fn(
  (function(int, named int $a, named string $b): void) $_,
): void {}

function immediate_application_reordered_accepts(): void {
  ((named $a, named $b) ==> {
    wants_int($a);
    wants_string($b);
  })(b = "s", a = 1);
}

function interleaved_positional_and_named_accepts(): void {
  take_interleaved_fn(($x, named $name) ==> {
    wants_int($x);
    wants_string($name);
  });
}

function take_interleaved_fn(
  (function(named string $name, int): void) $_,
): void {}

function take_two_positional_fn(
  (function(named string $name, int, bool): void) $_,
): void {}

function multiple_interleaved_positional_accepts(): void {
  take_two_positional_fn(($x, named $name, $flag) ==> {
    wants_int($x);
    wants_string($name);
    wants_bool($flag);
  });
}

function wants_bool(bool $x): void {}

function immediate_interleaved_accepts(): void {
  (($x, named $b, $flag, named $a) ==> {
    wants_int($x);
    wants_bool($flag);
    wants_int($a);
    wants_string($b);
  })(1, false, a = 2, b = "s");
}

function broad_explicit_annotations_accept(): void {
  take_named_fn((named arraykey $b, named num $a) ==> {
    wants_arraykey($b);
    wants_num($a);
  });
}

function wants_arraykey(arraykey $x): void {}
function wants_num(num $x): void {}

function partly_annotated_reordered_accepts(): void {
  take_named_fn((named string $b, named $a) ==> wants_int($a));
}

function take_optional_named_fn(
  (function(optional named int $a, named string $b): void) $_,
): void {}

function optional_named_context_accepts(): void {
  take_optional_named_fn((named $b, named $a = 0) ==> {
    wants_int($a);
    wants_string($b);
  });
}

function immediate_named_before_positional_accepts(): void {
  (($x, named $a) ==> {
    wants_int($x);
    wants_string($a);
  })(a = "s", 1);
}

function take_positional_variadic_fn(
  (function(named int $a, named string $b, int...): void) $_,
): void {}

function reordered_with_positional_collector_accepts(): void {
  take_positional_variadic_fn((named $b, named $a, ...$rest) ==> {
    wants_int($a);
    wants_string($b);
    wants_int_vec($rest);
  });
}

function wants_int_vec(vec<int> $x): void {}

function take_fixed_positional_fn(
  (function(int, string, named bool $flag): void) $_,
): void {}

function extra_positionals_flow_into_collector(): void {
  take_fixed_positional_fn((named $flag, ...$rest) ==> {
    wants_bool($flag);
    wants_arraykey_vec($rest);
  });
}

function wants_arraykey_vec(vec<arraykey> $x): void {}

function take_heterogeneous_variadic_fn(
  (function(int, string..., named bool $flag): void) $_,
): void {}

function non_tail_collector_with_expected_variadic_accepts(): void {
  take_heterogeneous_variadic_fn((...$values, named $flag) ==> {
    hh_expect_equivalent<vec<(int | string)>>($values);
    hh_expect_equivalent<bool>($flag);
  });
  take_heterogeneous_variadic_fn((...$values, named bool $flag) ==> {
    hh_expect_equivalent<vec<(int | string)>>($values);
    hh_expect_equivalent<bool>($flag);
  });
}

function non_tail_collector_with_fixed_expected_accepts(): void {
  take_fixed_positional_fn((...$values, named $flag) ==> {
    hh_expect_equivalent<vec<(int | string)>>($values);
    hh_expect_equivalent<bool>($flag);
  });
  take_fixed_positional_fn((...$values, named bool $flag) ==> {
    hh_expect_equivalent<vec<(int | string)>>($values);
    hh_expect_equivalent<bool>($flag);
  });
}
//
