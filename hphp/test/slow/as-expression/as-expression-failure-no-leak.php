<?hh

function as_fail(mixed $x): void {
  try {
    $x as vec<int>;
  } catch (TypeAssertionException $_) {}
}

function memory_delta(mixed $x): int {
  $before = memory_get_usage();
  for ($i = 0; $i < 10000; $i++) {
    as_fail($x);
  }
  return memory_get_usage() - $before;
}

<<__EntryPoint>>
function main(): void {
  memory_delta('a');
  echo memory_delta('a') < 100000 ? "no leak\n" : "leak\n";
}
