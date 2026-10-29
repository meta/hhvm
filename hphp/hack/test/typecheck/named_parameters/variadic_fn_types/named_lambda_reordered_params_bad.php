<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

function take_named_variadic_fn(
  (function(named shape('value' => int) $name, named int...): void) $_,
): void {}

function wants_int(int $x): void {}
function wants_string(string $x): void {}

function named_variadic_extra_body_mistype_rejects(): void {
  take_named_variadic_fn((named $extra = 0, named $name, named int...) ==> {
    wants_string($extra);
    wants_string($name['value']);
  });
}

function named_variadic_explicit_annotations_remain_authoritative(): void {
  take_named_variadic_fn((named num $extra = 0, named shape('value' => int) $name, named int...) ==> {
    wants_int($extra);
  });
}

function named_variadic_extra_default_mistype_rejects(): void {
  take_named_variadic_fn((named $extra = "s", named $name, named int...) ==> {});
}

function take_named_num_variadic_fn(
  (function(named shape('value' => int) $name, named num...): void) $_,
): void {}

function named_variadic_context_not_narrowed_to_default(): void {
  take_named_num_variadic_fn((named $extra = 0, named $name, named num...) ==> {
    wants_int($extra);
  });
}

function take_both_collectors_fn(
  (function(int, string, named bool $flag, named int...): void) $_,
): void {}

function take_both_variadic_collectors_fn(
  (function(int, string..., named bool $flag, named int...): void) $_,
): void {}

function both_collectors_extra_positional_body_mistype_rejects(): void {
  take_both_collectors_fn((...$rest, named $flag, named int...) ==> {
    wants_int_vec($rest);
    wants_int($flag);
  });
  take_both_variadic_collectors_fn((...$rest, named $flag, named int...) ==> {
    wants_int_vec($rest);
    wants_int($flag);
  });
}

function wants_int_vec(vec<int> $x): void {}

function immediately_called_named_collector_body_mistype_rejects(): void {
  (($x, named $optional = 0, named arraykey...) ==> {
    wants_string($x);
    wants_string($optional);
  })(1, extra = "s");
}
//
