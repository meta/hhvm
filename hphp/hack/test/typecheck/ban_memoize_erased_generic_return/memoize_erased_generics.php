<?hh

// getter<int>() and getter<arraykey>() share one cached Vector.
final class Break2 {
  public static function run(): int {
    $vi = self::getter<int>();
    $va = self::getter<arraykey>();
    $va[] = 'foo';
    return $vi[0];
  }

  <<__Memoize(#MakeICInaccessible)>>
  public static function getter<T>(): Vector<T> {
    return Vector {};
  }
}

// Break3<T> is invariant in T, so one cached Break3 serves every T2.
class Break3<T> {
  public static function run(): int {
    $vi = self::getter<int>();
    $va = self::getter<arraykey>();
    $va->writeAT('foo');
    return $vi->readAT();
  }

  <<__Memoize(#MakeICInaccessible)>>
  public static function getter<T2>(): Break3<T2> {
    return new Break3();
  }

  public ?T $t = null;
  public function writeAT(T $in): void {
    $this->t = $in;
  }
  public function readAT(): T {
    return $this->t as nonnull;
  }
}

<<__Memoize>>
function memo_mutable_in_awaitable<T>(): Awaitable<Map<string, T>> {
  return async {
    return Map {};
  };
}

<<__Memoize>>
function memo_contravariant<T>(): (function(T): void) {
  return (T $_) ==> {};
}

class LSB {
  <<__MemoizeLSB>>
  public static function lsb<T>(): Vector<T> {
    return Vector {};
  }
}

// A type alias counts with its declared variance, as in class variance
// checks, so an alias declared without `+T` is invariant.
type InvariantVec<T> = vec<T>;

<<__Memoize>>
function memo_invariant_alias<T>(): InvariantVec<T> {
  return vec[];
}
