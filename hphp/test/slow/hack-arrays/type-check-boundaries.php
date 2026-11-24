<?hh

use namespace HH\Lib\{C, Vec};

<<__NEVER_INLINE>>
function checked_contains_key(
  readonly KeyedContainer<arraykey, mixed> $value,
  arraykey $key,
): bool {
  return array_key_exists($key, $value);
}

<<__NEVER_INLINE>>
function checked_vec(mixed $value): vec<mixed> {
  return __hhvm_intrinsics\launder_value($value);
}

<<__NEVER_INLINE>>
function checked_dict(mixed $value): dict<arraykey, mixed> {
  return __hhvm_intrinsics\launder_value($value);
}

<<__NEVER_INLINE>>
function checked_keyset(mixed $value): keyset<arraykey> {
  return __hhvm_intrinsics\launder_value($value);
}

<<__NEVER_INLINE>>
function checked_sort(mixed $value): vec<mixed> {
  $result = vec($value);
  sort(inout $result);
  return __hhvm_intrinsics\launder_value($result);
}

<<__NEVER_INLINE>>
function checked_shuffle(mixed $value): vec<mixed> {
  $result = vec($value);
  shuffle(inout $result);
  return __hhvm_intrinsics\launder_value($result);
}

<<__NEVER_INLINE>>
function check_array_value(mixed $value, string $label): void {
  $present = count($value) !== 0;
  invariant(
    checked_contains_key($value, 0) === $present,
    '%s: KeyedContainer parameter',
    $label,
  );
  invariant(
    C\contains_key($value, 0) === $present,
    '%s: C\\contains_key',
    $label,
  );
  invariant(!C\contains_key($value, 'missing'), '%s: absent key', $label);

  if ($value is vec<_>) {
    invariant(checked_vec($value) === $value, '%s: vec return', $label);
  } else if ($value is dict<_, _>) {
    invariant(checked_dict($value) === $value, '%s: dict return', $label);
  } else {
    invariant(checked_keyset($value) === $value, '%s: keyset return', $label);
  }

  $expected = $present ? vec[0, 1, 2] : vec[];
  invariant(checked_sort($value) === $expected, '%s: sort return', $label);
  invariant(Vec\sort($value) === $expected, '%s: Vec\\sort', $label);
  $shuffled = checked_shuffle($value);
  sort(inout $shuffled);
  invariant($shuffled === $expected, '%s: shuffle return', $label);
  $shuffled = Vec\shuffle($value);
  sort(inout $shuffled);
  invariant($shuffled === $expected, '%s: Vec\\shuffle', $label);
}

<<__EntryPoint>>
function main(): void {
  $dynamic_vec = vec[];
  $dynamic_dict = dict[];
  $dynamic_keyset = keyset[];
  foreach (__hhvm_intrinsics\launder_value(vec[2, 0, 1]) as $value) {
    $dynamic_vec[] = $value;
    $dynamic_dict[$value] = $value;
    $dynamic_keyset[] = $value;
  }
  $empty_vec = $dynamic_vec;
  $empty_dict = $dynamic_dict;
  $empty_keyset = $dynamic_keyset;
  for ($i = 0; $i < 3; ++$i) {
    array_pop(inout $empty_vec);
    array_pop(inout $empty_dict);
    array_pop(inout $empty_keyset);
  }
  $cases = dict[
    'literal vec' => vec[2, 0, 1],
    'literal dict' => dict[2 => 2, 0 => 0, 1 => 1],
    'literal keyset' => keyset[2, 0, 1],
    'literal empty vec' => vec[],
    'literal empty dict' => dict[],
    'literal empty keyset' => keyset[],
    'dynamic vec' => $dynamic_vec,
    'dynamic dict' => $dynamic_dict,
    'dynamic keyset' => $dynamic_keyset,
    'dynamic empty vec' => $empty_vec,
    'dynamic empty dict' => $empty_dict,
    'dynamic empty keyset' => $empty_keyset,
  ];
  for ($iteration = 0; $iteration < 32; ++$iteration) {
    foreach (__hhvm_intrinsics\launder_value($cases) as $label => $value) {
      check_array_value($value, $label);
      check_array_value(unserialize(serialize($value)), $label.' unserialized');
      apc_store('type-check-boundaries', $value);
      $success = false;
      $cached = apc_fetch('type-check-boundaries', inout $success);
      invariant($success, '%s: APC fetch', $label);
      check_array_value($cached, $label.' APC');
    }
  }
  apc_delete('type-check-boundaries');
  echo "Array parameter and return checks passed\n";
}
