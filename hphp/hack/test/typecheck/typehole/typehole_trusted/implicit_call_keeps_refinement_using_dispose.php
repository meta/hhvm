<?hh

final class Holder {
  public ?int $x = 1;
}

final class Disposer implements IDisposable {
  public function __construct(private Holder $holder) {}
  public function __dispose(): void {
    $this->holder->x = null;
  }
}

// The end of the using block runs __dispose(), which clears $holder->x.
function using_dispose(Holder $holder): int {
  using (new Disposer($holder)) {
    if ($holder->x is null) {
      return 0;
    }
  }
  return $holder->x;
}

// Refinements made after the using block are unaffected.
function using_then_refine(Holder $holder): int {
  using (new Disposer($holder)) {
  }
  if ($holder->x is null) {
    return 0;
  }
  return $holder->x;
}
