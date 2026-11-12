<?hh

class C {
  public arraykey $k = 0;
}

function takes_string(string $_): void {}

async function values(): AsyncIterator<(string, string)> {
  yield tuple('v', 's');
}

// foreach await reads $c->k for the index before any write, so the key is 0.
async function repro(): Awaitable<void> {
  $c = new C();
  $dd = dict<string, string>[];
  foreach (values() await as list($dd[$c->k], $c->k)) {
    foreach ($dd as $key => $_) {
      takes_string($key);
    }
  }
}

<<__EntryPoint>>
function main(): void {
  \HH\Asio\join(repro());
}
