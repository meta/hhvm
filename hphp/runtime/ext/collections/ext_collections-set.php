<?hh
<<file:__EnableUnstableFeatures('readonly')>>

namespace {

/** An iterator implementation for iterating over a Set.
 */
<<__NativeData>>
final class SetIterator<T as arraykey> implements HH\Iterator<T> {

  public function __construct()[]: void {}

  /** Returns the current value that the iterator points to.
   * @return mixed
   */
  <<__Native>>
  public function current()[]: T;

  /** @return mixed
   */
  <<__Native>>
  public function key()[]: T;

  /** Returns true if the iterator points to a valid value, returns false
   * otherwise.
   * @return bool
   */
  <<__Native>>
  public function valid()[]: bool;

  /** Advance this iterator forward one position.
   */
  <<__Native>>
  public function next()[write_props]: void;

  /** Move this iterator back to the first position.
   */
  <<__Native>>
  public function rewind()[write_props]: void;
}

} // empty namespace
namespace HH {

/** An ordered set-style collection.
 */
final class Set<T as arraykey> implements \MutableSet<T> {

  /** Returns a Set built from the values produced by the specified Iterable.
   * @param mixed $iterable
   */
  <<__Native>>
  public function __construct(mixed $iterable = null)[]: void;

  /** Returns true if the Set is empty, false otherwise.
   * @return bool
   */
  public readonly function isEmpty()[]: bool { return $this->count() === 0; }

  /** Returns the number of values in the Set.
   * @return int
   */
  <<__Native>>
  public readonly function count()[]: int;

  /** Returns an Iterable that produces the values from this Set.
   * @return object
   */
  /* HH_FIXME[2049] */
  public function items()[]: \LazyIterableView {
    /* HH_FIXME[2049] */
    return new \LazyIterableView($this);
  }


  /** Returns a Vector built from the keys of this Set.
   * @return object
   */
  public function keys()[]: Vector<T> { return $this->values(); }

  /** Returns a Vector built from the values of this Set.
   * @return object
   */
  <<__Native>>
  public function values()[]: Vector<T>;

  /** Returns a lazy iterable view of this Set.
   * @return object
   */
  /* HH_FIXME[2049] */
  public function lazy()[]: \LazyKeyedIterableView {
    /* HH_FIXME[2049] */
    return new \LazyKeyedIterableView($this);
  }

  <<__Native>>
  private function clearNative()[write_props]: void;

  /** Removes all values from the Set.
   * @return object
   */
  public function clear()[write_props]: this {
    $this->clearNative();
    return $this;
  }

  /** Returns true if the specified value is present in the Set, returns false
   * otherwise.
   * @param mixed $val
   * @return bool
   */
  <<__Native>>
  public readonly function contains(mixed $val)[]: bool;

  <<__Native>>
  private function removeNative(mixed $val)[write_props]: void;

  /** Removes the specified value from this Set.
   * @param mixed $val
   * @return object
   */
  public function remove(mixed $val)[write_props]: this {
    $this->removeNative($val);
    return $this;
  }

  <<__Native>>
  private function addNative(mixed $val)[write_props]: void;
  /** Adds the specified value to this Set.
   * @param mixed $val
   * @return object
   */
  // TODO(T125046752) Should be `T`
  public function add(mixed $val)[write_props]: this {
    $this->addNative($val);
    return $this;
  }

  <<__Native>>
  private function addAllNative(mixed $iterable)[write_props]: void;

  /** Adds the values produced by the specified Iterable to this Set.
   * @param mixed $iterable
   * @return object
   */
  public function addAll(mixed $iterable)[write_props]: this {
    $this->addAllNative($iterable);
    return $this;
  }

  <<__Native>>
  public function addAllKeysOfNative(mixed $container)[write_props]: void;

  /** Adds the keys of the specified KeyedContainer to this Set.
   * @param mixed $container
   * @return object
   */
  public function addAllKeysOf(mixed $container)[write_props]: this {
    $this->addAllKeysOfNative($container);
    return $this;
  }

  /** Instructs this Set to grow its capacity to accommodate the given number of
   * elements. The caller is expected to make the appropriate add/addAll calls
   * to fill that reserved capacity.
   * @param mixed $sz
   */
  <<__Native>>
  public function reserve(int $sz)[]: void;

  public function toVArray()[]: varray<T> {
    return HH\FIXME\UNSAFE_CAST<vec<mixed>, vec<T>>(vec($this));
  }

  /** Returns a darray built from the values from this Set, dict[val1 => val1,
   * val2 => val2, ...]. This maintains set-like semantics in dict[] land:
   * O(1) membership test with `array_has_key($a['key'])` and iteration with
   *  `foreach($a as $member)`.
   * @return darray
   */
  public function toDArray()[]: darray<T, T> {
    return HH\FIXME\UNSAFE_CAST<dict<arraykey, mixed>, dict<T, T>>(dict($this));
  }

  /** Returns a Vector built from the values of this Set.
   * @return object
   */
  <<__Native>>
  public function toVector()[]: Vector<T>;

  /** Returns a ImmVector built from the values of this Set.
   * @return object
   */
  <<__Native>>
  public function toImmVector()[]: ImmVector<T>;

  /** Returns a Map built from the keys and values of this Set.
   * @return object
   */
  <<__Native>>
  /* HH_FIXME[2049] */
  public function toMap()[]: Map<T, T>;

  /** Returns a ImmMap built from the keys and values of this Set.
   * @return object
   */
  <<__Native>>
  /* HH_FIXME[2049] */
  public function toImmMap()[]: ImmMap<T, T>;

  /** Returns a copy of this Set.
   * @return object
   */
  public function toSet()[]: this {
    return new self($this);
  }

  /** Returns a ImmSet built from the values of this Set.
   * @return object
   */
  <<__Native>>
  public function toImmSet()[]: ImmSet<T>;

  /** Returns an immutable version of this collection.
   * @return object
   */
  public function immutable()[]: ImmSet<T> { return $this->toImmSet(); }

  /** Returns a varray built from the values from this Set.
   * @return varray
   */
  <<__Native>>
  public function toKeysArray()[]: varray<T>;

  /** Returns a varray built from the values from this Set.
   * @return varray
   */
  <<__Native>>
  public function toValuesArray()[]: varray<T>;

  /** Returns an iterator that points to beginning of this Set.
   * @return object
   */
  <<__Native>>
  public function getIterator()[]: Iterator<T>;

  /** Returns a Set of the values produced by applying the specified callback on
   * each value from this Set.
   * @param mixed $callback
   * @return object
   */
  public function map<To as arraykey>(
    (function(T)[_]: To) $callback,
  )[ctx $callback]: Set<To> {
    $ret = keyset[];
    foreach ($this as $v) {
      $ret[] = $callback($v);
    }
    return new self($ret);
  }

  /** Returns a Set of the values produced by applying the specified callback on
   * each key and value from this Set.
   * @param mixed $callback
   * @return object
   */
  public function mapWithKey<To as arraykey>(
    (function(T, T)[_]: To) $callback,
  )[ctx $callback]: Set<To> {
    $ret = keyset[];
    foreach ($this as $k => $v) {
      $ret[] = $callback($k, $v);
    }
    return new self($ret);
  }

  /** Returns a Set of all the values from this Set for which the specified
   * callback returns true.
   * @param mixed $callback
   * @return object
   */
  public function filter(
    (function(T)[_]: bool) $callback,
  )[ctx $callback]: Set<T> {
    $ret = keyset[];
    foreach ($this as $v) {
      if ($callback($v)) {
        $ret[] = $v;
      }
    }
    return new self($ret);
  }

  /** Returns a Set of all the values from this Set for which the specified
   * callback returns true.
   * @param mixed $callback
   * @return object
   */
  public function filterWithKey(
    (function(T, T)[_]: bool) $callback,
  )[ctx $callback]: Set<T> {
    $ret = keyset[];
    foreach ($this as $k => $v) {
      if ($callback($k, $v)) {
        $ret[] = $v;
      }
    }
    return new self($ret);
  }

  /** Ensures that this Set contains only values for which the specified callback
   * returns true.
   * @param mixed $callback
   * @return object
   */
  public function retain(
    (function(T)[_]: bool) $callback,
  )[ctx $callback]: this {
    foreach ($this as $k => $v) {
      if (!$callback($v)) {
        /* HH_FIXME[4390] This function should be `write_props` */
        $this->remove($k);
      }
    }
    return $this;
  }

  /** Ensures that this Set contains only keys/values for which the specified
   * callback returns true when passed the key and the value.
   * @param mixed $callback
   * @return object
   */
  public function retainWithKey(
    (function(T, T)[_]: bool) $callback,
  )[ctx $callback]: this {
    foreach ($this as $k => $v) {
      if (!$callback($k, $v)) {
        /* HH_FIXME[4390] This function should be `write_props` */
        $this->remove($k);
      }
    }
    return $this;
  }

  /** Returns a Iterable produced by combined the specified Iterables pair-wise.
   * @param mixed $iterable
   * @return object
   */
  <<__Native>>
  public function zip(mixed $iterable)[]: Set<nothing>;

  /** Returns a Set containing the first n values of this Set.
   * @param mixed $n
   * @return object
   */
  <<__Native>>
  public function take(mixed $n)[]: Set<T>;

  /** Returns a Set containing the values of this Set up to but not including the
   * first value that produces false when passed to the specified callback.
   * @param mixed $callback
   * @return object
   */
  public function takeWhile(
    (function(T)[_]: bool) $callback,
  )[ctx $callback]: Set<T> {
    $ret = keyset[];
    foreach ($this as $v) {
      if (!$callback($v)) {
        break;
      }
      $ret[] = $v;
    }
    return new self($ret);
  }

  /** Returns a Set containing all values except the first n of this Set.
   * @param mixed $n
   * @return object
   */
  <<__Native>>
  public function skip(mixed $n)[]: Set<T>;

  /** Returns a Set containing all the values of this Set excluding the first
   * values that produces true when passed to the specified callback.
   * @param mixed $fn
   * @return object
   */
  public function skipWhile(
    (function(T)[_]: bool) $fn,
  )[ctx $fn]: Set<T> {
    $ret = keyset[];
    $skipping = true;
    foreach ($this as $v) {
      if ($skipping) {
        if ($fn($v)) {
          continue;
        }
        $skipping = false;
      }
      $ret[] = $v;
    }
    return new self($ret);
  }

  /** Returns a Set containing the specified range of values from this Set. The
   * range is specified by two non-negative integers: a starting position and a
   * length.
   * @param mixed $start
   * @param mixed $len
   * @return object
   */
  <<__Native>>
  public function slice(mixed $start,
                        mixed $len)[]: Set<T>;

  /** Builds a new Vector by concatenating the elements of this Set with the
   * elements of the specified Iterable.
   * @param mixed $iterable
   * @return object
   */
  <<__Native>>
  public function concat(mixed $iterable)[]: Vector<mixed>;

  /** Returns the first value from this Set, or null if this Set is empty.
   * @return mixed
   */
  <<__Native>>
  public readonly function firstValue()[]: ?T;

  /** Returns the first key from this Set, or null if this Vector is empty.
   * @return mixed
   */
  public readonly function firstKey()[]: ?T {
    return $this->firstValue();
  }

  /** Returns the last value from this Set, or null if this Set is empty.
   * @return mixed
   */
  <<__Native>>
  public readonly function lastValue()[]: ?T;

  /** Returns the last key from this Set, or null if this Set is empty.
   * @return mixed
   */
  public readonly function lastKey()[]: ?T {
    return $this->lastValue();
  }

  <<__Native>>
  private function removeAllNative(mixed $iterable)[write_props]: void;

  /** @param mixed $iterable
   * @return object
   */
  public function removeAll(mixed $iterable)[write_props]: this {
    $this->removeAllNative($iterable);
    return $this;
  }

  /** @param mixed $iterable
   * @return object
   */
  public function difference(mixed $iterable): this {
    // This isn't really a difference method, it lies
    $this->removeAllNative($iterable);
    return $this;
  }

  /** @return string
   */
  public function __toString()[]: string { return "Set"; }

  /** Returns a Set built from the values produced by the specified Iterable.
   * @param mixed $iterable
   * @return object
   */
  <<__Native>>
  public static function fromItems(mixed $iterable)[]: Set<arraykey>;

  /** Returns a Set built from the keys of the specified container.
   * @param mixed $container
   * @return object
   */
  <<__Native>>
  public static function fromKeysOf(mixed $container)[]: Set<arraykey>;

  /** Returns a Set built from the values from the specified array.
   * @param mixed $arr
   * @return object
   */
  <<__Native>>
  public static function fromArray(mixed $arr): Set<arraykey>;

  /** Returns a Set built from the values from the specified arrays.
   * @return object
   */
  public static function fromArrays<T_ as arraykey>(
    AnyArray<arraykey, T_>... $argv
  )[]: Set<T_> {
    $ret = keyset[];
    foreach ($argv as $arr) {
      if (!\HH\is_any_array($arr)) {
        throw new \InvalidArgumentException("Parameters must be array-likes");
      }
      foreach ($arr as $value) {
        $ret[] = $value;
      }
    }
    return new Set($ret);
  }
}

/** An immutable ordered set-style collection.
 */
final class ImmSet<T as arraykey> implements \ConstSet<T> {

  /** Returns a ImmSet built from the values produced by the specified Iterable.
   * @param mixed $iterable
   */
  <<__Native>>
  public function __construct(mixed $iterable = null)[]: void;

  /** Returns true if the ImmSet is empty, false otherwise.
   * @return bool
   */
  public readonly function isEmpty()[]: bool { return $this->count() === 0; }

  /** Returns the number of values in the ImmSet.
   * @return int
   */
  <<__Native>>
  public readonly function count()[]: int;

  /** Returns an Iterable that produces the values from this ImmSet.
   * @return object
   */
  /* HH_FIXME[2049] */
  public function items()[]: \LazyIterableView {
    /* HH_FIXME[2049] */
    return new \LazyIterableView($this);
  }


  /** Returns a ImmVector built from the keys of this ImmSet.
   * @return object
   */
  public function keys()[]: ImmVector<T> { return $this->values(); }

  /** Returns an ImmVector built from the values of this ImmSet.
   * @return object
   */
  <<__Native>>
  public function values()[]: ImmVector<T>;

  /** Returns a lazy iterable view of this ImmSet.
   * @return object
   */
  /* HH_FIXME[2049] */
  public function lazy()[]: \LazyKeyedIterableView {
    /* HH_FIXME[2049] */
    return new \LazyKeyedIterableView($this);
  }

  /** Returns true if the specified value is present in the ImmSet, returns false
   * otherwise.
   * @param mixed $val
   * @return bool
   */
  <<__Native>>
  public function contains(mixed $val)[]: bool;

  public function toVArray()[]: varray<T> {
    return vec($this);
  }

  /** Returns a darray built from the values from this ImmSet, dict[val1 =>
   * val1, val2 => val2, ...]. This maintains set-like semantics in dict[]
   * land: O(1) membership test with `array_has_key($a['key'])` and iteration
   * with `foreach($a as $member)`.
   * @return darray
   */
  public function toDArray()[]: darray<T, T> {
    return dict($this);
  }

  /** Returns a Vector built from the values of this ImmSet.
   * @return object
   */
  <<__Native>>
  public function toVector()[]: Vector<T>;

  /** Returns a ImmVector built from the values of this ImmSet.
   * @return object
   */
  <<__Native>>
  public function toImmVector()[]: ImmVector<T>;

  /** Returns a Map built from the keys and values of this ImmSet.
   * @return object
   */
  <<__Native>>
  /* HH_FIXME[2049] */
  public function toMap()[]: Map<T, T>;

  /** Returns a ImmMap built from the keys and values of this ImmSet.
   * @return object
   */
  <<__Native>>
  /* HH_FIXME[2049] */
  public function toImmMap()[]: ImmMap<T, T>;

  /** Returns a Set built from the values of this ImmSet.
   * @return object
   */
  <<__Native>>
  public function toSet()[]: Set<T>;

  /** Returns an immutable version of this collection.
   * @return object
   */
  public function toImmSet()[]: this {
    return $this;
  }

  /** Returns an immutable version of this collection.
   * @return object
   */
  public function immutable()[]: this {
    return $this;
  }

  /** Returns a varray built from the values from this ImmSet.
   * @return varray
   */
  <<__Native>>
  public function toKeysArray()[]: varray<T>;

  /** Returns a varray built from the values from this ImmSet.
   * @return varray
   */
  <<__Native>>
  public function toValuesArray()[]: varray<T>;

  /** Returns an iterator that points to beginning of this ImmSet.
   * @return object
   */
  <<__Native>>
  public function getIterator()[]: Iterator<T>;

  /** Returns a ImmSet of the values produced by applying the specified callback
   * on each value from this ImmSet.
   * @param mixed $callback
   * @return object
   */
  public function map<To as arraykey>(
    (function(T)[_]: To) $callback,
  )[ctx $callback]: ImmSet<To> {
    $ret = keyset[];
    foreach ($this as $v) {
      $ret[] = $callback($v);
    }
    return new self($ret);
  }

  /** Returns a ImmSet of the values produced by applying the specified callback
   * on each key and value from this ImmSet.
   * @param mixed $callback
   * @return object
   */
  public function mapWithKey<To as arraykey>(
    (function(T, T)[_]: To) $callback,
  )[ctx $callback]: ImmSet<To> {
    $ret = keyset[];
    foreach ($this as $k => $v) {
      $ret[] = $callback($k, $v);
    }
    return new self($ret);
  }

  /** Returns a ImmSet of all the values from this ImmSet for which the specified
   * callback returns true.
   * @param mixed $callback
   * @return object
   */
  public function filter(
    (function(T)[_]: bool) $callback,
  )[ctx $callback]: ImmSet<T> {
    $ret = keyset[];
    foreach ($this as $v) {
      if ($callback($v)) {
        $ret[] = $v;
      }
    }
    return new self($ret);
  }

  /** Returns a ImmSet of all the values from this ImmSet for which the specified
   * callback returns true.
   * @param mixed $callback
   * @return object
   */
  public function filterWithKey(
    (function(T, T)[_]: bool) $callback,
  )[ctx $callback]: ImmSet<T> {
    $ret = keyset[];
    foreach ($this as $k => $v) {
      if ($callback($k, $v)) {
        $ret[] = $v;
      }
    }
    return new self($ret);
  }

  /** Returns an Iterable produced by combining the specified Iterables
   * pair-wise.
   * @param mixed $iterable
   * @return object
   */
  <<__Native>>
  public function zip(mixed $iterable)[]: ImmSet<nothing>;

  /** Returns a ImmSet containing the first n values of this ImmSet.
   * @param mixed $n
   * @return object
   */
  <<__Native>>
  public function take(mixed $n)[]: ImmSet<T>;

  /** Returns a ImmSet containing the values of this ImmSet up to but not
   * including the first value that produces false when passed to the specified
   * callback.
   * @param mixed $callback
   * @return object
   */
  public function takeWhile(
    (function(T)[_]: bool) $callback,
  )[ctx $callback]: ImmSet<T> {
    $ret = keyset[];
    foreach ($this as $v) {
      if (!$callback($v)) {
        break;
      }
      $ret[] = $v;
    }
    return new self($ret);
  }

  /** Returns a ImmSet containing all values except the first n of this ImmSet.
   * @param mixed $n
   * @return object
   */
  <<__Native>>
  public function skip(mixed $n)[]: ImmSet<T>;

  /** Returns a ImmSet containing all the values of this ImmSet excluding the
   * first values that produces true when passed to the specified callback.
   * @param mixed $fn
   * @return object
   */
  public function skipWhile((function(T)[_]: bool) $fn)[ctx $fn]: ImmSet<T> {
    $ret = keyset[];
    $skipping = true;
    foreach ($this as $v) {
      if ($skipping) {
        if ($fn($v)) {
          continue;
        }
        $skipping = false;
      }
      $ret[] = $v;
    }
    return new self($ret);
  }

  /** Returns a ImmSet containing the specified range of values from this ImmSet.
   * The range is specified by two non-negative integers: a starting position
   * and a length.
   * @param mixed $start
   * @param mixed $len
   * @return object
   */
  <<__Native>>
  public function slice(mixed $start, mixed $len)[]: ImmSet<T>;

  /** Builds a new ImmVector by concatenating the elements of this ImmSet with
   * the elements of the specified Iterable.
   * @param mixed $iterable
   * @return object
   */
  <<__Native>>
  public function concat(mixed $iterable)[]: ImmVector<T>;

  /** Returns the first value from this ImmSet, or null if this ImmSet is empty.
   * @return mixed
   */
  <<__Native>>
  public readonly function firstValue()[]: ?T;

  /** Returns the first key from this ImmSet, or null if this ImmSet is empty.
   * @return mixed
   */
  public readonly function firstKey()[]: ?T {
    return $this->firstValue();
  }

  /** Returns the last value from this ImmSet, or null if this ImmSet is empty.
   * @return mixed
   */
  <<__Native>>
  public readonly function lastValue()[]: ?T;

  /** Returns the last key from this ImmSet, or null if this ImmSet is empty.
   * @return mixed
   */
  public readonly function lastKey()[]: ?T {
    return $this->lastValue();
  }

  /** @return string
   */
  public function __toString()[]: string { return "ImmSet"; }

  /** Returns a ImmSet built from the values produced by the specified Iterable.
   * @param mixed $iterable
   * @return object
   */
  <<__Native>>
  public static function fromItems(mixed $iterable)[]: ImmSet<T>;

  /** Returns a ImmSet built from the keys of the specified container.
   * @param mixed $container
   * @return object
   */
  <<__Native>>
  public static function fromKeysOf(mixed $container)[]: ImmSet<T>;

  /** Returns a ImmSet built from the values from the specified array.
   * @param mixed $arr
   * @return object
   */
  <<__Native>>
  public static function fromArray(mixed $arr): ImmSet<arraykey>;

  /** Returns a ImmSet built from the values from the specified arrays.
   * @return object
   */
  public static function fromArrays<T_ as arraykey>(
    AnyArray<arraykey, T_>... $argv
  )[]: ImmSet<T_> {
    $ret = keyset[];
    foreach ($argv as $arr) {
      if (!\HH\is_any_array($arr)) {
        throw new \InvalidArgumentException("Parameters must be array-likes");
      }
      foreach ($arr as $value) {
        $ret[] = $value;
      }
    }
    return new ImmSet($ret);
  }
}

} // namespace HH
