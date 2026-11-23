<?hh

function foo(
  shape("foo" => int, "foo" => string) $x
): void {
  echo "ok\n";
}

<<__EntryPoint>>
function main(): void {
  foo(shape('foo' => 'value'));
}
