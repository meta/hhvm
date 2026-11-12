<?hh

function takes_a(shape('a' => int, ...) $s): int {
  return $s['a'];
}

function remove_from_generic<T as shape('a' => int, ...)>(T $s): void {
  Shapes::removeKey(inout $s, 'a');
  takes_a($s);
}

<<__EntryPoint>>
function main(): void {
  remove_from_generic(shape('a' => 1));
}
