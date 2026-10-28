<?hh

// A rhs typed as a tuple of the list's arity can't be too short, so these
// list() assignments are not throw points and the catch sees $x as int.
function list_tuple_param((int, string) $t): int {
  $x = 0;
  try {
    $x = 'foo';
    list($_a, $_b) = $t;
    $x = 1;
  } catch (Exception $_) {
    return $x;
  }
  return $x;
}

function list_tuple_literal(): int {
  $x = 0;
  try {
    $x = 'foo';
    list($_a, $_b) = tuple(1, 2);
    $x = 1;
  } catch (Exception $_) {
    return $x;
  }
  return $x;
}

function list_tuple_nested((int, (int, int)) $t): int {
  $x = 0;
  try {
    $x = 'foo';
    list($_a, list($_b, $_c)) = $t;
    $x = 1;
  } catch (Exception $_) {
    return $x;
  }
  return $x;
}

function list_tuple_bound<T as (int, string)>(T $t): int {
  $x = 0;
  try {
    $x = 'foo';
    list($_a, $_b) = $t;
    $x = 1;
  } catch (Exception $_) {
    return $x;
  }
  return $x;
}

function list_tuple_like(~(int, string) $t): int {
  $x = 0;
  try {
    $x = 'foo';
    list($_a, $_b) = $t;
    $x = 1;
  } catch (Exception $_) {
    return $x;
  }
  return $x;
}

// Optional and variadic elements may be absent, so these still throw.
function list_tuple_optional((int, optional int) $t): int {
  $x = 0;
  try {
    $x = 'foo';
    list($_a, $_b) = $t;
    $x = 1;
  } catch (Exception $_) {
    return $x;
  }
  return $x;
}

function list_tuple_variadic((int...) $t): int {
  $x = 0;
  try {
    $x = 'foo';
    list($_a, $_b) = $t;
    $x = 1;
  } catch (Exception $_) {
    return $x;
  }
  return $x;
}

// A lambda parameter is still an unsolved type variable here, so this throws.
async function map_async<Tv, Tr>(
  Traversable<Tv> $_t,
  (function(Tv): Awaitable<Tr>) $_f,
): Awaitable<vec<Tr>> {
  return vec[];
}

function takes_int(int $_): void {}

async function list_tuple_lambda_param(vec<(int, string)> $v): Awaitable<void> {
  await map_async($v, async $t ==> {
    $x = 0;
    try {
      $x = 'foo';
      list($_a, $_b) = $t;
      $x = 1;
    } catch (Exception $_) {
      takes_int($x);
    }
  });
}

// Only tuples flow into T by the list(), but the lambda's return then adds
// vec<int>, and the second call destructures vec[1], so this must throw.
function twice<T>(T $init, (function(T): T) $f): void {
  $f($f($init));
}

function list_tuple_lambda_own_return(): void {
  twice(tuple(1, 'a'), $t ==> {
    $x = 0;
    try {
      $x = 'foo';
      list($_a, $_b) = $t;
      $x = 1;
    } catch (Exception $_) {
      takes_int($x);
    }
    return vec[1];
  });
}

// Same, with vec<int> added by a lambda checked after this one.
function apply<T>(T $init, (function(T): void) $use, (function(int): T) $mk): void {
  $use($init);
  $use($mk(1));
}

function list_tuple_lambda_later_lambda(): void {
  apply(
    tuple(1, 'a'),
    $t ==> {
      $x = 0;
      try {
        $x = 'foo';
        list($_a, $_b) = $t;
        $x = 1;
      } catch (Exception $_) {
        takes_int($x);
      }
    },
    $i ==> vec[$i],
  );
}
