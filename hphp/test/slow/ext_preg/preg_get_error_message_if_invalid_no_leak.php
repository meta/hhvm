<?hh

function memory_delta(string $pattern): int {
  $before = memory_get_usage();
  for ($i = 0; $i < 10000; $i++) {
    preg_get_error_message_if_invalid($pattern);
  }
  return memory_get_usage() - $before;
}

<<__EntryPoint>>
function main(): void {
  memory_delta('/o');
  echo memory_delta('/o') < 100000 ? "no leak\n" : "leak\n";
}
