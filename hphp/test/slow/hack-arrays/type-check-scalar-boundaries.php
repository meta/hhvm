<?hh

<<__NEVER_INLINE>>
function string_parameter(string $value): mixed {
  return $value;
}

<<__NEVER_INLINE>>
function string_return(mixed $value): string {
  return __hhvm_intrinsics\launder_value($value);
}

<<__NEVER_INLINE>>
function bool_parameter(bool $value): mixed {
  return $value;
}

<<__NEVER_INLINE>>
function bool_return(mixed $value): bool {
  return __hhvm_intrinsics\launder_value($value);
}

<<__EntryPoint>>
function main(): void {
  $dynamic = __hhvm_intrinsics\launder_value('type').'-check';
  $cases = vec['', 'type-check', $dynamic, substr($dynamic, 0, 0), false, true];
  for ($iteration = 0; $iteration < 32; ++$iteration) {
    foreach (__hhvm_intrinsics\launder_value($cases) as $value) {
      foreach (vec[$value, unserialize(serialize($value))] as $input) {
        if ($value is string) {
          invariant(string_parameter($input) === $value, 'string parameter');
          invariant(string_return($input) === $value, 'string return');
        } else {
          invariant(bool_parameter($input) === $value, 'bool parameter');
          invariant(bool_return($input) === $value, 'bool return');
        }
      }
    }
  }
  echo "String and bool parameter and return checks passed\n";
}
