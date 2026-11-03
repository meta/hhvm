<?hh

type RenamedTail<TItem> = shape(Vector<TItem>...);
type SameTail<T> = shape(Vector<T>...);
type NamedField<T> = shape('v' => Vector<T>);

<<__Memoize>>
function direct_tail<T>(): shape(Vector<T>...) {
  return shape('v' => Vector {});
}

<<__Memoize>>
function renamed_alias_tail<T>(): RenamedTail<T> {
  return shape('v' => Vector {});
}

<<__Memoize>>
function same_alias_tail<T>(): SameTail<T> {
  return shape('v' => Vector {});
}

<<__Memoize>>
function named_field<T>(): NamedField<T> {
  return shape('v' => Vector {});
}

<<__Memoize>>
function covariant_tail<<<__Explicit>> T>(): shape(vec<T>...) {
  return shape();
}

function not_memoized<T>(): shape(Vector<T>...) {
  return shape('v' => Vector {});
}

<<__Memoize>>
function reified_tail<reify T>(): shape(Vector<T>...) {
  return shape('v' => Vector {});
}
