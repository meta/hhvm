<?hh

// `foreach (... await ...)` calls the iterator's next() without checking its
// coeffects, so the pure function calls a [defaults] method.
final class Hgoldstein implements AsyncIterator<int> {
  public function current(): int {
    var_dump("UH OH");
    return 42;
  }
  public async function next(): Awaitable<?(mixed, int)> {
    return null;
  }
  public function rewind(): void {}
  public function valid(): bool {
    return true;
  }
  public function key(): int {
    return 13;
  }
}

async function hgoldstein_foobar()[]: Awaitable<int> {
  $hg = new Hgoldstein();
  foreach ($hg await as $x) {
    return $x;
  }
  invariant_violation('!!!');
}

<<__EntryPoint>>
async function main(): Awaitable<void> {
  await hgoldstein_foobar();
}
