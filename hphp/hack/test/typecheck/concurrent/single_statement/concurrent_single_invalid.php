<?hh

async function invalid_single_statement(): Awaitable<void> {
  concurrent {
    $value = 1;
  }
}
