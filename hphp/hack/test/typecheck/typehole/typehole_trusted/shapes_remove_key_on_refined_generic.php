<?hh

function takes_a(shape('a' => int, ...) $s): int {
  return $s['a'];
}

function remove_from_refined<T>(T $s): void {
  if ($s is shape('a' => int, ...)) {
    Shapes::removeKey(inout $s, 'a');
    takes_a($s);
  }
}

<<__EntryPoint>>
function main(): void {
  remove_from_refined(shape('a' => 1));
}
