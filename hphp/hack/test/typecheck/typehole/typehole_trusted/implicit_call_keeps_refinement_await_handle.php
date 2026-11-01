<?hh

final class Holder {
  public ?int $x = 1;
}

// Another concurrent branch can clear $holder->x while this one is suspended.
async function gen_read(
  Holder $holder,
  Awaitable<void> $wait,
): Awaitable<int> {
  if ($holder->x is nonnull) {
    await $wait;
    return $holder->x;
  }
  return 0;
}

async function gen_pure()[]: Awaitable<void> {}

// Even a pure await suspends, and other code may run meanwhile.
async function gen_await_pure(Holder $holder): Awaitable<int> {
  if ($holder->x is nonnull) {
    await gen_pure();
    return $holder->x;
  }
  return 0;
}

// Refinements made after the await are unaffected.
async function gen_refine_after(Holder $holder): Awaitable<int> {
  await gen_pure();
  if ($holder->x is nonnull) {
    return $holder->x;
  }
  return 0;
}

async function gen_local(Holder $holder): Awaitable<int> {
  $x = $holder->x;
  if ($x is nonnull) {
    await gen_pure();
    return $x;
  }
  return 0;
}
