<?hh

// getInstanceKey() ignores the contents, so an int box and a string box share
// one cache entry and the cached int comes back typed as string.
final class BadMemoParamBox<+T> implements IMemoizeParam {
  public function getInstanceKey(): string {
    return '';
  }

  public function __construct(private T $thing) {}

  public function get()[]: T {
    return $this->thing;
  }
}

<<__Memoize(#MakeICInaccessible)>>
function bad_get_out_of_box<T>(BadMemoParamBox<T> $box): T {
  return $box->get();
}

<<__Memoize>>
function bad_nested<T>(vec<BadMemoParamBox<T>> $boxes): ?T {
  return null;
}

type BoxAlias<T> = BadMemoParamBox<T>;

<<__Memoize>>
function bad_alias<T>(BoxAlias<T> $box): T {
  return $box->get();
}

// Values are keyed by value.
<<__Memoize>>
function memo_value<T>(T $x): T {
  return $x;
}

<<__Memoize>>
function memo_vec_value<T>(vec<T> $x): ?T {
  return null;
}

// Reified generics are part of the key.
<<__Memoize>>
function memo_reified<reify T>(BadMemoParamBox<T> $box): T {
  return $box->get();
}

// T doesn't reach the result.
<<__Memoize>>
function memo_not_returned<T>(BadMemoParamBox<T> $box): int {
  return 1;
}

final class PlainKey implements IMemoizeParam {
  public function getInstanceKey(): string {
    return '';
  }
}

<<__Memoize>>
function memo_plain_param<T>(PlainKey $k, T $x): T {
  return $x;
}
