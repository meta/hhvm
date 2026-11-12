<?hh

interface I {
  public static function make(): int;
}

trait IT {
  require implements I;
}

function call_make(class<I> $c): int {
  return $c::make();
}

<<__EntryPoint>>
function main(): void {
  call_make(IT::class);
}
