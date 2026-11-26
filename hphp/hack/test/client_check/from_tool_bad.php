<?hh
function always_true(vec<int> $items): void {
  if (null !== $items) {}
}
function wrong_return(): int { return 'wrong'; }
