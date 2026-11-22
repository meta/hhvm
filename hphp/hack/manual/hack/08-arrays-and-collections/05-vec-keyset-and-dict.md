# Vec Keyset & Dict

`vec`, `keyset` and `dict` are value types. Any mutation produces a
new value, and does not modify the original value.

These types are referred to as 'Hack arrays'. Prefer using these types
whenever you're unsure.

## `vec`

A `vec` is an ordered, iterable data structure. It is created with
the `vec[]` syntax.

```hack
// Creating a vec.
function get_items(): vec<string> {
  $items = vec['a', 'b', 'c'];
  return $items;
}
```

`vec`s can be accessed with the following syntax.

```hack
$items = vec['a', 'b', 'c'];

// Accessing items by index.
$items[0]; // 'a'
$items[3]; // throws OutOfBoundsException

// Accessing items that might be out-of-bounds.
idx($items, 0); // 'a'
idx($items, 3); // null
idx($items, 3, 'default'); // 'default'

// Modifying items. These operations set $items
// to a modified copy, and do not modify the original value.
$items[0] = 'xx'; // vec['xx', 'b', 'c']
$items[] = 'd'; // vec['xx', 'b', 'c', 'd']

// Getting the length.
C\count($items); // 4

// Seeing if a vec contains a value or index.
C\contains($items, 'a'); // true
C\contains_key($items, 2); // true

// Iterating.
foreach ($items as $item) {
  echo $item;
}
// Iterating with the index.
foreach ($items as $index => $item) {
  echo $index; // e.g. 0
  echo $item;  // e.g. 'a'
}

// Equality checks. Elements are recursively compared with ===.
vec[1] === vec[1]; // true
vec[1, 2] === vec[2, 1]; // false

// Combining vecs.
Vec\concat(vec[1], vec[2, 3]); // vec[1, 2, 3]

// Removing items at an index.
$items = vec['a', 'b', 'c'];
$n = 1;
Vec\concat(Vec\take($items, $n), Vec\drop($items, $n + 1)); // vec['a', 'c']

// Converting from an Iterable.
vec(keyset[10, 11]); // vec[10, 11]
vec(Vector { 20, 21 }); // vec[20, 21]
vec(dict['key1' => 'value1']); // vec['value1']

// Type checks.
$items is vec<_>; // true
```

## `keyset`

A `keyset` is an ordered data structure without duplicates. It is
created with the `keyset[]` syntax.

A `keyset` can only contain `string` or `int` values. `keyset`s are
ordered according to the insertion order.


```hack
// Creating a keyset.
function get_items(): keyset<string> {
  $items = keyset['a', 'b', 'c'];
  return $items;
}
```

`keyset`s can be accessed with the following syntax.

```hack
$items = keyset['a', 'b', 'c'];

// Checking if a keyset contains a value.
C\contains($items, 'a'); // true

// Adding/removing items. These operations set $items to a modified copy,
// and do not modify the original value.
$items[] = 'd'; // keyset['a', 'b', 'c', 'd']
$items[] = 'a'; // keyset['a', 'b', 'c', 'd']
unset($items['b']); // keyset['a', 'c', 'd']

// Getting the length.
C\count($items); // 3

// Iterating.
foreach ($items as $item) {
  echo $item;
}

// Equality checks. === returns false if the order does not match.
keyset[1] === keyset[1]; // true
keyset[1, 2] === keyset[2, 1]; // false
Keyset\equal(keyset[1, 2], keyset[2, 1]); // true

// Combining keysets.
Keyset\union(keyset[1, 2], keyset[2, 3]); // keyset[1, 2, 3]

// Converting from an Iterable.
keyset(vec[1, 2, 1]); // keyset[1, 2]
keyset(Vector { 20, 21 }); // keyset[20, 21]
keyset(dict['key1' => 'value1']); // keyset['value1']

// Type checks.
$items is keyset<_>; // true
```

## `dict`

A `dict` is an ordered key-value data structure. It is
created with the `dict[]` syntax.

Keys must be `string`s or `int`s. `dict`s are ordered according to the
insertion order.

```hack
// Creating a dict.
function get_items(): dict<string, int> {
  $items = dict['a' => 1, 'b' => 3];
  return $items;
}
```

`dicts`s can be accessed with the following syntax.

```hack
$items = dict['a' => 1, 'b' => 3];

// Accessing items by key.
$items['a']; // 1
$items['foo']; // throws OutOfBoundsException

// Accessing keys that may be absent.
idx($items, 'a'); // 1
idx($items, 'z'); // null
idx($items, 'z', 'default'); // 'default'

// Inserting, updating or removing values in a dict. These operations
// set $items to a modified copy, and do not modify the original value.
$items['a'] = 42; // dict['a' => 42, 'b' => 3]
$items['z'] = 100; // dict['a' => 42, 'b' => 3, 'z' => 100]
unset($items['b']); // dict['a' => 42, 'z' => 100]

// Getting the keys.
Vec\keys(dict['a' => 1, 'b' => 3]); // vec['a', 'b']

// Getting the values.
vec(dict['a' => 1, 'b' => 3]); // vec[1, 3]

// Getting the length.
C\count($items); // 2

// Checking if a dict contains a key or value.
C\contains_key($items, 'a'); // true
C\contains($items, 3); // true

// Iterating values.
foreach ($items as $value) {
  echo $value; // e.g. 1
}
// Iterating keys and values.
foreach ($items as $key => $value) {
  echo $key;   // e.g. 'a'
  echo $value; // e.g. 1
}

// Equality checks. === returns false if the order does not match.
dict[] === dict[]; // true
dict[0 => 10, 1 => 11] === dict[1 => 11, 0 => 10]; // false
Dict\equal(dict[0 => 10, 1 => 11], dict[1 => 11, 0 => 10]); // true

// Combining dicts (last item wins).
Dict\merge(dict[10 => 1, 20 => 2], dict[30 => 3, 10 => 0]);
// dict[10 => 0, 20 => 2, 30 => 3];

// Converting from an Iterable.
dict(vec['a', 'b']); // dict[0 => 'a', 1 => 'b']
dict(Map {'a' => 5}); // dict['a' => 5]

// Type checks.
$items is dict<_, _>; // true
```

If you want different keys to have different value types, or if you
want a fixed set of keys, consider using a `shape` instead.
