<?hh

<<__ALWAYS_INTERP, __NEVER_INLINE>>
async function interp_leaf(int $value): Awaitable<int> {
  return $value;
}

<<__ALWAYS_INLINE>>
async function inlined_wrapper(int $value): Awaitable<int> {
  return await interp_leaf($value);
}

<<__NEVER_INLINE>>
function sync_caller(int $value): int {
  return HH\Asio\join(inlined_wrapper($value));
}

<<__EntryPoint>>
function main(): void {
  var_dump(sync_caller(__hhvm_intrinsics\launder_value(42)));
}
