<?hh

function remove_a<T as shape(?'a' => int, ...)>(inout T $s): void {
  Shapes::removeKey(inout $s, 'a');
}

<<__EntryPoint>>
function main(): void {
  $s = shape('a' => 1);
  remove_a(inout $s);
  $s['a'];
}
