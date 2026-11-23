<?hh

function foo(
  /* HH_FIXME[2015] */
  shape("foo" => int, "foo" => string) $x
): void {
  echo "ok\n";
}

<<__EntryPoint>>
function main(): void {
  foo(shape('foo' => 'value'));
}
