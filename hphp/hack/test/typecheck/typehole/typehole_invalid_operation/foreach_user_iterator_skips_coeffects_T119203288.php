<?hh

// `foreach` calls the iterator's methods without checking their coeffects, so
// the pure foobar() runs current()'s impure code; HHVM doesn't stop it either.
final class Hgoldstein implements Iterator<int> {
  public function current(): int {
    var_dump("UH OH");
    return 42;
  }
  public function next(): void {}
  public function rewind(): void {}
  public function valid(): bool {
    return true;
  }
  public function key(): int {
    return 13;
  }
}

function foobar()[]: int {
  $hg = new Hgoldstein();
  foreach ($hg as $x) {
    return $x;
  }
  invariant_violation('!!!');
}

<<__EntryPoint>>
function main(): void {
  foobar();
}
