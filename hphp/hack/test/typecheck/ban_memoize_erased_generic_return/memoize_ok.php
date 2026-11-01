<?hh

// Reified generics are part of the memoization key.
<<__Memoize>>
function memo_reified<reify T>(): Vector<T> {
  return Vector {};
}

// Only covariant uses: no value of T can be produced without an argument.
<<__Memoize>>
function memo_covariant<<<__Explicit>> T>(): ImmVector<T> {
  return ImmVector {};
}

<<__Memoize>>
function memo_vec<<<__Explicit>> T>(): vec<T> {
  return vec[];
}

<<__Memoize>>
function memo_unused<T>(): int {
  return 1;
}

<<__Memoize>>
function memo_plain(): Vector<int> {
  return Vector {};
}

<<__Memoize>>
async function gen_memo_covariant<<<__Explicit>> T>(): Awaitable<?T> {
  return null;
}

// A type alias counts with its declared variance.
type MyVec<+T> = vec<T>;

<<__Memoize>>
function memo_alias<<<__Explicit>> T>(): MyVec<T> {
  return vec[];
}

// Not memoized.
function not_memoized<T>(): Vector<T> {
  return Vector {};
}

final class C {
  // The cache is per instance for instance methods.
  <<__Memoize>>
  public function inst(): Vector<int> {
    return Vector {};
  }
}
