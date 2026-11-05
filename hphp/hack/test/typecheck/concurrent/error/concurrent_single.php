<?hh

async function value(): Awaitable<int> {
  return 1;
}

async function single_await(): Awaitable<void> {
  concurrent {
    await value();
  }
}
