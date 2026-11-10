<?hh
<<file: __EnableUnstableFeatures('named_parameters', 'union_intersection_type_hints')>>

function take_named_variadic_fn(
  (function(named shape('value' => int) $name, named int...): void) $_,
): void {}

function wants_int(int $x): void {}

function named_variadic_extra_accepts(): void {
  take_named_variadic_fn((named $extra = 0, named $another = 1, named $name, named int...) ==> {
    wants_int($extra);
    wants_int($another);
    wants_int($name['value']);
  });
}

function take_named_num_variadic_fn(
  (function(named shape('value' => int) $name, named num...): void) $_,
): void {}

function named_variadic_context_broader_than_default_accepts(): void {
  take_named_num_variadic_fn((named $extra = 0, named $name, named num...) ==> {
    wants_num($extra);
    wants_int($name['value']);
  });
}

function wants_num(num $x): void {}

function take_both_collectors_fn(
  (function(int, string, named bool $flag, named int...): void) $_,
): void {}

function take_both_variadic_collectors_fn(
  (function(int, string..., named bool $flag, named int...): void) $_,
): void {}

function both_collectors_preserve_extra_positionals_accepts(): void {
  take_both_collectors_fn((...$rest, named $flag, named int...) ==> {
    hh_expect_equivalent<vec<(int | string)>>($rest);
    hh_expect_equivalent<bool>($flag);
  });
  take_both_variadic_collectors_fn((...$rest, named $flag, named int...) ==> {
    hh_expect_equivalent<vec<(int | string)>>($rest);
    hh_expect_equivalent<bool>($flag);
  });
}

function immediately_called_named_collector_accepts(): void {
  (($x, named $optional_value = 0, named arraykey...) ==> {
    wants_int($x);
    wants_int($optional_value);
  })(1, extra = "s");
}

abstract class PositionalVariadicOverrideBase {
  abstract public function m(int ...$args, named string $name): void;
}

class PositionalVariadicOverride extends PositionalVariadicOverrideBase {
  public function m(int ...$args, named string $name): void {}
}
