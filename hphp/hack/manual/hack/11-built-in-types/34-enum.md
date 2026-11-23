# Enum

Use an enum (enumerated type) to create a set of named, constant, immutable values.

In Hack, enums are limited to `int` or `string` (as an [`Arraykey`](/hack/built-in-types/arraykey)), or other `enum` values.

## Quick Start
To access an enum's value, use its full name, as in `Colors::Blue` or `Permission::Read`.

```hack
<<__AllowUncheckedEnumValues>>
enum Colors: int {
  Red = 3;
  Green = 5;
  Blue = 10;
  Default = 3; // duplicate value is allowed by the attribute
}
```

```hack
enum Permission: string {
  Read = 'R';
  Write = 'W';
  Execute = 'E';
  Delete = 'D';
}
```

Additionally, by using the [`as`](/hack/expressions-and-operators/type-assertions#enforcing-types-with-as-and-as) operator to enforce type, you can initialize your enum with static expressions that reference other enum values. Computed expressions like those below cannot be statically checked for duplicate values, so the enum must use `<<__AllowUncheckedEnumValues>>`.

```hack
<<__AllowUncheckedEnumValues>>
enum BitFlags: int as int {
  F1 = 1; // value 1
  F2 = BitFlags::F1 << 1; // value 2
  F3 = BitFlags::F2 << 1; // value 4
  F4 = 4 + 3; // value 7
}
```

## Full Example
With an enum, we can create a placement-direction system with names like `Top`, `Bottom`, `Left`, `Right`, and `Center`, then direct output accordingly to write text to the top, bottom, left, right, or center of a window.

```hack
enum Position: int {
  Top = 0;
  Bottom = 1;
  Left = 2;
  Right = 3;
  Center = 4;
}

function writeText(string $text, Position $pos): void {
  switch ($pos) {
    case Position::Top:
      // ...
      break;
    case Position::Center:
      // ...
      break;
    case Position::Right:
      // ...
      break;
    case Position::Left:
      // ...
      break;
    case Position::Bottom:
      // ...
      break;
  }
}

<<__EntryPoint>>
function main(): void {
  writeText("Hello", Position::Bottom);
  writeText("Today", Position::Left);
}
```

## Library Methods
All enums implement these public static methods.

### `getValues()` / `getNames()`
Returns a `dict` of enum constant values and their names.

* `getValues()` returns a `dict` where the keys are the enum names and the values are the enum constant values.
  * In the example below, the keys/values would be: `"Top" => 0`, `"Bottom" => 1`, etc.
* `getNames()` returns a `dict`, but is flipped: the keys are the enum constant values and the values are the enum's named constants.
  * Following the same example, the keys/values would be: `0 => "Top"`, `1 => "Bottom"`, etc.
  * Because a `dict` *can not* contain duplicate keys, when you call `getNames()`—the static method that returns a `dict` and flips an enum's constant values *to* keys—there is a possiblity of creating a `dict` with duplicates, resulting in an `HH\InvariantException`. In this situation, one safe option for discarding duplicates (and keeping the most recent of every duplicate) is `Dict\flip`.

```hack
enum Position: int {
  Top = 0;
  Bottom = 1;
  Left = 2;
  Right = 3;
  Center = 4;
}

<<__EntryPoint>>
function main(): void {
  $names = Position::getNames();
  echo " Position::getNames() ---\n";
  foreach ($names as $key => $value) {
    echo "\tkey >$key< has value >$value<\n";
  }

  $values = Position::getValues();
  echo "Position::getValues() ---\n";
  foreach ($values as $key => $value) {
    echo "\tkey >$key< has value >$value<\n";
  }

  Dict\flip(Position::getValues()); // safe flip of values as keys
}
```

### `assert()` / `coerce()`
`assert($value)` checks if a value exists in an enum, and if it does, returns the value; if the value does not exist, throws an `UnexpectedValueException`.

`coerce($value)` checks if a value exists in an enum, and if it does, returns the value; if the value does not exist, returns `null`.

```hack
enum Bits: int {
  B1 = 2;
  B2 = 4;
  B3 = 8;
 }

<<__EntryPoint>>
function main(): void {
  Bits::assert(2); // 2
  Bits::assert(16); // UnexpectedValueException

  Bits::coerce(2); // 2
  Bits::coerce(2.0); // null
  Bits::coerce(16); // null
}
```

**Note:** Both library methods will, when no exact match is found, attempt to do a cast to the other `arraykey` type. If the cast is not reversible / lossless, or the resulting value is still not a member of the enum after the cast, the failure result occurs, where a failure for assert is throwing an `UnexpectedValueException` and a failure for `coerce` is returning null.

### `assertAll()`
`assertAll($traversable)` calls `assert($value)` on every element of the traversable (e.g. [Hack Arrays](/hack/arrays-and-collections/vec-keyset-and-dict)); if at least one value does not exist, throws an `UnexpectedValueException`.


```hack
enum Bits: int {
  B1 = 2;
  B2 = 4;
  B3 = 8;
 }

<<__EntryPoint>>
function main(): void {
  $all_values = vec[2, 4, 8];
  $some_values = vec[2, 4, 16];
  $no_values = vec[32, 64, 128];

  Bits::assertAll($all_values); // vec[2, 4, 8]
  Bits::assertAll($some_values); // throws on 16, UnexpectedValueException
  Bits::assertAll($no_values); // throws on 32, UnexpectedValueException
}
```

### `isValid()`
`isValid($value)` checks if a value exists in an enum, and if it does, returns `true`; if the value does not exist, it returns `false`.

```hack
enum Bits: int {
  B1 = 2;
  B2 = 4;
  B3 = 8;
 }

<<__EntryPoint>>
function main(): void {
  \var_dump(Bits::isValid(2));
  \var_dump(Bits::isValid(2.0));
  \var_dump(Bits::isValid("2.0"));
  \var_dump(Bits::isValid(8));
}
```

## `is` / `as`
The operators [`is`](/hack/expressions-and-operators/type-assertions#checking-types-with-is) and [`as`/`?as`](/hack/expressions-and-operators/type-assertions#enforcing-types-with-as-and-as) behave similarly, but not exactly, to `isValid()` (similar to `is`) and `assert()`/`coerce()` (similar to `as`/`?as`).

For `is`/`as`/`?as` refinement, the operators validate that a value is a part of a given enum. **Caution:** These operators may perform implicit int/string coercion of enum values to preserve compatibility with `isValid()`.

```hack
enum MyEnum: int {
  FOO = 1;
}

<<__EntryPoint>>
function main(): void {
1 is MyEnum; // true
1 as MyEnum; // 1

42 is MyEnum; // false
42 as MyEnum; // TypeAssertionException

'foo' is MyEnum; // false
'foo' as MyEnum; // TypeAssertionException

'1' is MyEnum; // CAUTION - true
'1' as MyEnum; // CAUTION - '1'
}
```

## Enum Inclusion
You can define an enum to include all of the constants of another enum with the `use` keyword.

In the following example, `enum` `F` contains all of the constants of `enum` `E1` and `enum` `E2`.

```hack
enum E1: int as int {
  A = 0;
}
enum E2: int as int {
  B = 1;
}
enum F: int {
  // same-line alternative: use E1, E2;
  use E1;
  use E2;
  C = 2;
}
```

Enum Inclusion is subject to a few restrictions:
* **Order**: All `use` statements must precede enum constant declarations.
* **Uniqueness**: All constant names across all enums must be unique.
* **Subtype Relation**: In the above example, `E1` and `E2` are not considered subtypes of `F`; that is, the Hack Typechecker rejects passing `E1::A` or `E2::B` to a function that expects an argument of type `F`.

**Note:** Library functions like `getNames()` and `getValues()` perform a post-order traversal of all included enums.
