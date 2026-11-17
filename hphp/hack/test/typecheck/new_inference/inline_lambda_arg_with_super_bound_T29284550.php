<?hh

function launder(): bool {
  return false;
}

function myfunc<T as nonnull, Tr super T>(
  (function(): ?T) $f,
  Tr $default,
): Tr {
  return $f() ?? $default;
}

function breakit(): <<__Soft>> int {
  return myfunc(() ==> launder() ? 42 : 'I broke it', -1);
}
