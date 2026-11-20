<?hh

final class Node {
  public ?Node $next = null;
  public vec<int> $payload = vec[];
  public function __construct(public int $id, int $n) {
    for ($i = 0; $i < $n; $i++) $this->payload[] = $id * 31 + $i;
  }
}

function make_ring(int $base, int $len, int $n): Node {
  $first = new Node($base, $n);
  $cur = $first;
  for ($i = 1; $i < $len; $i++) {
    $next = new Node($base + $i, $n);
    $cur->next = $next;
    $cur = $next;
  }
  $cur->next = $first;
  return $first;
}

function ring_sum(Node $first): int {
  $sum = 0;
  $cur = $first;
  do {
    foreach ($cur->payload as $v) $sum += $v;
    $cur = $cur->next as nonnull;
  } while ($cur !== $first);
  return $sum;
}

function str_of(int $i): string {
  $len = $i % 1009 === 0 ? 900000
    : ($i % 97 === 0 ? 100000 + $i : ($i * 7919) % 4096 + 1);
  return str_repeat(chr(65 + $i % 26), $len);
}

<<__EntryPoint>>
function main(): void {
  $strs = dict[];
  $rings = vec[];
  $sums = vec[];
  for ($round = 0; $round < 6; $round++) {
    for ($i = 0; $i < 300; $i++) {
      make_ring($round * 1000 + $i, 4, 1 + $i % 40);
    }
    for ($i = 0; $i < 400; $i++) {
      $k = $round * 400 + $i;
      $strs[$k] = str_of($k);
    }
    $ring = make_ring($round, 3, 16);
    $rings[] = $ring;
    $sums[] = ring_sum($ring);
    gc_collect_cycles();
    gc_check_heap();
    foreach ($strs as $k => $_) {
      if (($k * 7 + $round) % 4 === 0) unset($strs[$k]);
    }
  }
  gc_collect_cycles();
  gc_check_heap();

  $bad = 0;
  foreach ($strs as $k => $s) {
    if ($s !== str_of($k)) $bad++;
  }
  foreach ($rings as $j => $ring) {
    if (ring_sum($ring) !== $sums[$j]) $bad++;
  }
  echo count($strs), " strings, ", count($rings), " rings, ", $bad, " bad\n";
}
