<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

function take_named_fn((function(named int $a, named string $b): void) $_): void {}
function wants_int(int $x): void {}
function wants_string(string $x): void {}

function reordered_lambda_body_mistype_rejects(): void {
  take_named_fn((named $b, named $a) ==> {
    wants_string($a);
    wants_int($b);
  });
}

function explicit_annotations_remain_authoritative(): void {
  take_named_fn((named arraykey $b, named num $a) ==> {
    wants_string($b);
    wants_int($a);
  });
}

function missing_name_rejects(): void {
  take_named_fn((named $a) ==> {});
}

function wrong_name_rejects(): void {
  take_named_fn((named $a, named $wrong) ==> {});
}

function immediate_body_mistype_rejects(): void {
  ((named $a, named $b) ==> {
    wants_string($a);
    wants_int($b);
  })(b = "s", a = 1);
}

function take_optional_named_fn(
  (function(optional named int $a, named string $b): void) $_,
): void {}

function optional_named_default_mistype_rejects(): void {
  take_optional_named_fn((named $b, named $a = "s") ==> {});
}

function take_extra_positional_fn(
  (function(int, bool, named string $name): void) $_,
): void {}

function missing_positionals_preserves_named_type(): void {
  take_extra_positional_fn((named $name) ==> wants_string($name));
}

function take_fixed_positional_fn(
  (function(int, string, named bool $flag): void) $_,
): void {}

function extra_positional_collector_body_mistype_rejects(): void {
  take_fixed_positional_fn((named $flag, ...$rest) ==> {
    wants_int_vec($rest);
  });
}

function wants_int_vec(vec<int> $x): void {}

function take_heterogeneous_variadic_fn(
  (function(int, string..., named bool $flag): void) $_,
): void {}

function non_tail_collector_with_expected_variadic_body_mistype_rejects(): void {
  take_heterogeneous_variadic_fn((...$values, named $flag) ==> {
    wants_bool($values[0]);
    wants_int($flag);
  });
  take_heterogeneous_variadic_fn((...$values, named bool $flag) ==> {
    wants_bool($values[0]);
    wants_int($flag);
  });
}

function non_tail_collector_with_fixed_expected_body_mistype_rejects(): void {
  take_fixed_positional_fn((...$values, named $flag) ==> {
    wants_bool($values[0]);
    wants_int($flag);
  });
  take_fixed_positional_fn((...$values, named bool $flag) ==> {
    wants_bool($values[0]);
    wants_int($flag);
  });
}

function wants_bool(bool $x): void {}
//
