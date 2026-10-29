<?hh

<<__Memoize>>
function getter<T>(): Vector<T> {
  return Vector {};
}

function bad(Vector<int> $vi, Vector<arraykey> $va): void {
  if ($vi === $va) {
    $vi[] = 'str';
  }
}

function typed(Vector<int> $vi, Vector<arraykey> $va): int {
  bad($vi, $va);
  return $vi[0];
}

<<__EntryPoint>>
function main(): void {
  $vi = getter<int>();
  $va = getter<arraykey>();
  typed($vi, $va);
}
