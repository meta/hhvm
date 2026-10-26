<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

const int ITERATIONS = 4000;
const int PAYLOAD_BYTES = 4096;
// A missed decref retains about 80 MiB across the measured iterations.
const int MAX_GROWTH_BYTES = 1024 * 1024;

<<__NEVER_INLINE>>
function discard_only(named string...): void {}

<<__NEVER_INLINE>>
function discard_while_compacting(
  named string $middle = '',
  named string...
): void {}

<<__NEVER_INLINE>>
function discard_while_growing(
  named string $aa = '',
  named string $mm = '',
  named string $zz = '',
  named string...
): void {}

<<__NEVER_INLINE>>
function payload(int $seed): string {
  return str_repeat((string)($seed % 10), PAYLOAD_BYTES);
}

function exercise(int $iterations): void {
  for ($i = 0; $i < $iterations; ++$i) {
    discard_only(first=payload($i), second=payload($i));
    discard_while_compacting(
      zzz=payload($i),
      middle=payload($i),
      aaa=payload($i),
    );
    discard_while_growing(extra=payload($i));
  }
}

<<__EntryPoint>>
function main(): void {
  // Settle lazy allocations before measuring request-heap usage.
  exercise(100);
  $baseline = memory_get_usage();

  exercise(ITERATIONS);
  $growth = memory_get_usage() - $baseline;

  var_dump($growth < MAX_GROWTH_BYTES);
}
