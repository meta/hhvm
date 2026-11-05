<?hh

async function value(): Awaitable<int> {
  return 1;
}

async function single_await(): Awaitable<void> {
  concurrent {
    await value();
  }
}

async function single_assignment(): Awaitable<int> {
  concurrent {
    $result = await value();
    // await value();
  }
  return $result;
}

async function multiple_awaits_in_one_statement(): Awaitable<int> {
  concurrent {
    $result = await value() + await value();
  }
  return $result;
}

async function multiple_statements(): Awaitable<int> {
  concurrent {
    $first = await value();
    $second = await value();
  }
  return $first + $second;
}

function async_lambda(): (function(): Awaitable<void>) {
  return async () ==> {
    concurrent {
      await value();
    }
  };
}

async function suppressed_warning(): Awaitable<void> {
  /* HH_IGNORE[12048] */
  concurrent {
    await value();
  }
}

<<__SupportDynamicType>>
async function dynamic_body(int $input): Awaitable<int> {
  concurrent {
    $result = await value();
  }
  return $input + $result;
}
