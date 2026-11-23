# Error Codes

This page lists the most common error codes, and suggests how to fix
them. You can see the full list of error codes in
[error_map.ml](https://github.com/facebook/hhvm/blob/master/hphp/hack/src/errors/error_codes.ml).

## 1002: Top-level code

```hack no-extract
function foo(): void {}

/* HH_FIXME[1002] Top-level code isn't checked. */
echo "hello world\n";
```

**Why it's bad:** Top-level code is not type checked.

**Suggestions:** Put your code in a function and use the `__EntryPoint`
attribute.

## 2049: Unbound name

```hack
function foo(): void {
  /* HH_FIXME[4107] No such function (type checking). */
  /* HH_FIXME[2049] No such function (global name check). */
  nonexistent_function();
}
```

**Why it's bad:** This is usually a sign that a name is incorrect.

It may be useful for calling parts of the PHP standard library that
the global name check is not aware of.

**Suggestions:** Check your spelling. Use safe Hack APIs rather than
legacy PHP APIs.

## 2050: Undefined Variable

```hack
function foo(): mixed {
  /* HH_FIXME[2050] This variable doesn't exist. */
  return $no_such_var;
}
```

**Why it's bad:** This is usually a sign that a variable name is incorrect.

It may be useful for accessing PHP constants (such as `$GLOBALS` or `$_GET`)
which the typechecker is unaware of.

**Suggestions:** Check your spelling. Use safe Hack APIs rather than
legacy PHP APIs.

## 4006: Array append on an inappropriate type

```hack
function foo(mixed $m): void {
  /* HH_FIXME[4006] $m may not be an array. */
  $m[] = 1;
}
```

**Why it's bad:** Appending to other types (e.g. `int`) is undefined and
may throw an exception or convert the value to an array.

**Suggestions:** If the type isn't specific enough, use `as` (e.g. `as
vec<_>`) to perform a runtime type check.

## 4032: Missing return type

```hack
/* HH_FIXME[4030] Missing a return type declaration. */
function foo() {
  return 1;
}
```

**Why it's bad:** When the typechecker does not know the return type, it
cannot check operations on the value returned.

**Suggestions:** Add a return type to your function. If you're unsure of
the type, consider using `__Soft`. You may also want to consider a
`mixed` or `dynamic` return type.

## 4051: Accessing a shape with an invalid field name

```hack
function foo(shape(...) $s): void {
  /* HH_FIXME[4051] Invalid shape field name. */
  $value = $s[1.0];
}
```

**Why it's bad:** The runtime may coerce values and access other fields of
your shape. The typechecker also does not know what type `$value` has.

**Suggestions:** Use a valid shape key: a string (recommended), an
integer, or a class constant.

## 4053: Member not found

```hack
class MyClass {}

function takes_myclass(MyClass $c): void {
  /* HH_FIXME[4053] No such method. */
  $c->someMethod();
  /* HH_FIXME[4053] No such property. */
  $x = $c->someProperty;
}
```

**Why it's bad:** Accessing a non-existent method will cause a runtime
error. Accessing a non-existent property will log a notice and return null.

**Suggestions:** Ensure that the object you're accessing actually has the
type you're expecting.

## 4057: Missing shape field

```hack
function foo(): shape('x' => int) {
  /* HH_FIXME[4057] Missing the field `x`. */
  return shape();
}
```

**Why it's bad:** Returning a shape that's missing fields will cause
errors when code tries to access those fields later. Note that shape
fields are not enforced when calling or returning from functions.

**Suggestions:** Change your shape type to use optional fields.

## 4063: Nullable container access

```hack
function foo(?vec<int> $items): void {
  /* HH_FIXME[4063] $items can be null. */
  $x = $items[0];
}
```

**Why it's bad:** indexing a `null` returns null, leading to
runtime type errors later.

**Suggestions:** Check that the value is non-null with `nullthrows` or
assert with `$items as nonnull`.

## 4064: Accessing members on a nullable object

```hack
class MyClass {
  public int $x = 0;
  public function foo(): void {}
}

function foo(?MyClass $m): void {
  /* HH_FIXME[4064] Accessing a property on a nullable object. */
  $value = $m->x;

  /* HH_FIXME[4064] Calling a method on a nullable object. */
  $m->foo();
}
```

**Why it's bad:** Accessing a property or a method on `null` will throw an
exception.

**Suggestions:** Check that the value is non-null with `nullthrows` or
assert with `$m as nonnull`.

## 4101: Wrong number of type parameters

```hack
class MyBox<T> {
  public ?T $x = null;
}

/* HH_FIXME[4101] Missing a type parameter. */
class TooFewArguments extends MyBox {}

/* HH_FIXME[4101] Too many type parameters. */
class TooManyArguments extends MyBox<mixed, mixed> {}
```

**Why it's bad:** If the typechecker doesn't have full information about a
class declaration, it cannot fully check code that uses the class.

**Suggestions:** Add the necessary type parameters. You can usually use
`mixed` or `nothing` as the type parameter on base classes.

Note that this is only required for declarations. Hack can infer type
parameters inside function and method bodies.

## 4107: Unbound name (type checking)

```hack
function foo(): void {
  /* HH_FIXME[4107] No such function (type checking). */
  /* HH_FIXME[2049] No such function (global name check). */
  nonexistent_function();
}
```

**Why it's bad:** This is usually a sign that a name is incorrect.

It may be useful for calling parts of the PHP standard library that
the type checker is not aware of.

**Suggestions:** Check your spelling. Use safe Hack APIs rather than
legacy PHP APIs.

## 4108: Undefined shape field

```hack
function foo(shape('x' => int) $s): void {
  /* HH_FIXME[4108] No such field in this shape. */
  $value = $s['not_x'];
}
```

**Why it's bad:** Accessing an undefined field may throw an exception or
return an unexpected value (for open shapes).

**Suggestions:** Ensure that your shape type declaration has the fields
you're using.

## 4110: Bad type in expression

```hack
function takes_int(int $_): void {}

function foo(): void {
  /* HH_FIXME[4110] Passing incorrect type to a function. */
  takes_int("hello");
}
```

**Why it's bad:** Using the wrong type can result in runtime errors (for
enforced types), errors later (for unenforced types, such as erased
generics) or surprising coercions (e.g. for arithmetic).

**Suggestions:**

If the type is too broad (e.g. using `mixed`), use `as SpecificType`
to assert the specific runtime type. If you're not sure of the type,
consider using `<<__Soft>>` type hints on function signatures.

If the type is coming from very dynamic code, consider using the
`dynamic` type.

## 4166: Unknown field in shape (obsolete)

This error code is obsolete. The typechecker now produces error 4110
in these situations.

```
function test(shape('a' => string) $s): shape() {
  /* HH_FIXME[4166] Extra fields in the shape. */
  return $s;
}
```

**Why it's bad:** Passing extra fields in a shape can lead to surprising
results when converting shapes to arrays.

**Suggestions:** Use a field type declaration with optional fields instead.

## 4128: Using deprecated code

```hack
function foo_new(): void {}

<<__Deprecated("Use foo_new instead")>>
function foo_old(): void {}

function bar(): void {
  /* HH_FIXME[4128] Calling a deprecated function. */
  foo_old();
}
```

**Why it's bad:** Using functions or classes that have been marked as
deprecated prevents cleanup of old APIs.

**Suggestions:** [`__Deprecated`](/hack/attributes/predefined-attributes#__deprecated) takes a message which describes why
something is deprecated. Take a look at that message to learn the new
API.

## 4165: Accessing optional shape field

```hack
function foo(shape(?'x' => int) $s): void {
  /* HH_FIXME[4165] This field may not be present. */
  $value = $s['x'];
}
```

**Why it's bad:** This code will throw an exception if the shape doesn't
have this field.

**Suggestions:** Use `Shapes::idx` instead, so you can explicitly handle
the missing field.

## 4193: Illegal XHP child

```hack no-extract
use type Facebook\XHP\HTML\div;

function foo(mixed $m): void {
  /* HH_FIXME[4110] */ /* HH_FIXME[4193] $m may not be an XHPChild.*/
  $my_div = <div>{$m}</div>;
}
```

**Why it's bad:** XHP expects child elements to be instance of `XHPChild`.

**Suggestions:** Use `as` to assert a narrower type, or convert values to
a valid XHP child, such as a string.

## 4276: Truthiness check

```
$x = null;
if ($x) {
  echo "not null";
}

$y = '0';
if ($y) {
  echo "truthy string";
}
```

This error was [moved to a linter](https://github.com/hhvm/hhast), as it was making it harder to convert
partial mode files to strict. We still recommend avoiding this code style.

**Why it's bad:** Truthiness rules can be surprising. `''` is falsy, but
so is `'0'`.

**Suggestions:** Use `is null`, `=== ""` or `=== 0` when checking
for empty values.

## 4297: Type inference failed

```hack
class MyA {
  public function doStuff(): void {}
}

function foo(): void {
  /* HH_FIXME[4297] Cannot infer the type of $x. */
  $f = $x ==> $x->doStuff();

  $f(new MyA());
}
```

**Why it's bad:** If the type checker cannot infer the type, it cannot
check usage of values with that type.

This usually occurs with anonymous functions, but can also occur when
working with generic containers like `dict` or `vec`.

**Suggestions:** For anonymous functions, add a type annotation.

```
$f = (MyA $x) ==> $x->doStuff();
```

Alternatively, use a type assertion `($x as MyA)->doStuff()`.

When type inference fails on generic containers, add a type annotation
on the declaration.

```
$d = dict<string, string>[];
```

## 4323: Type constraint violation

```hack
/* HH_FIXME[4323] A dict must have arraykey, int or string keys. */
function foo(dict<mixed, bool> $d): void {}
```

**Why it's bad:** if a type has constraints on how it can be used, and you
break those constraints, it may not work as expected.

In this example, we define a `dict` with `mixed` keys. We can then
insert values that aren't `arraykey` types, leading to surprising
value conversions.

**Suggestions:** Look carefully at the error message to see what types are
supported for the generic you're using.

## 4324: Array access on a type that doesn't support indexing

```hack
function foo(int $m): void {
  /* HH_FIXME[4324] Indexing a type that isn't indexable. */
  $value = $m['foo'];
}
```

**Why it's bad:** Indexing values that don't support values can produce
surprising behavior. The runtime will log a warning and return null,
leading to runtime type errors later.

**Suggestions:** Refactor the code to use a Hack array or a
`KeyedContainer`.

**Note:** In previous versions of HHVM, error `4324` was known as error `4005`.
