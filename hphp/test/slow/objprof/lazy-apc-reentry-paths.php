<?hh

<<__EntryPoint>>
function main(): void {
  require_once __DIR__.'/lazy-apc-reentry.inc';
  test_lazy_reentry(true);
}
