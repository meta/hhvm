# Enum Class Label

## Values v. Bindings

With [enum types](/hack/built-in-types/enum) and [enum classes](/hack/built-in-types/enum-class), most of the focus is given to their values.
Expressions like `E::A` denote the value of `A` in `E`, but the fact that `A` was used to access it is lost.

```hack
<<__AllowUncheckedEnumValues>>
enum E: int {
  A = 42;
  B = 42;
}

function f(E $value): void {
  switch($value) {
    case E::A: echo "A "; break;
    case E::B: echo "B "; break;
  }
  echo $value . "\n";
}
```
In this example, both `f(E::A)` and `f(E::B)` will echo `A 42` because `E::A` and `E::B` are effectively the value `42` and nothing more.

## Enum Class Labels

Sometimes, the binding that was used to access a value from an enumeration is as important as the value itself. We might want to know that `A`
was used to access `E::A`. Enum types provides a partial solution to this with the `getNames` static method, but it is only
safe to call if all the values of the enumeration are distinct.

Enum classes provides a way to do this by using the newly introduced *Enum Class Label* expressions. For each value defined in an enum class, a corresponding
label is defined. A label is a handle to access the related value. Think of it as an indirect access. Consider the following example:

```hack file:label.hack
// We are using int here for readability but it works for any type
enum class E: int {
  int A = 42;
  int B = 42;
}
```

This enum class defines two constants:
- `E::A` of type `HH\MemberOf<E, int>`
- `E::B` of type `HH\MemberOf<E, int>`

The main addition of labels is a new **opaque** type: `HH\EnumClasslabel`. Let's first recall its full definition:

```hack
newtype Label<-TEnumClass, TType> = mixed;
```

This type has two generic arguments which are the same as `HH\MemberOf`:
- the first one is the enum class where the label is defined
- the second one is the data type indexed by the label

As an example, `HH\EnumClass\Label<E, int>` means that the label is from the enum class `E` and points to a value of type `int`.

Let us go back to our enum class `E`. Labels add more definitions to the mix:

- `E#A: HH\EnumClass\Label<E, int>` is the label to access `E::A`
- `E#B: HH\EnumClass\Label<E, int>` is the label to access `E::B`
- `E::nameOf` is a static method expecting a label and returning its string representation: `E::nameOf(E#A) === "A"`
- `E::valueOf` is a static method expecting a label and returning its value: `E::valueOf(E#A) === E::A`

So we can rewrite the earlier example in a more resilient way:

```hack file:label.hack
function full_print(\HH\EnumClass\Label<E, int> $label): void {
  echo E::nameOf($label) . " ";
  echo E::valueOf($label) . "\n";
}

function partial_print(\HH\MemberOf<E, int> $value): void {
  echo $value . "\n";
}
```
Now, `full_print(E#A)` will echo `A 42` and `full_print(E#B)` will echo `B 42`.

## Full v. Short Labels

We refer to labels like `E#A` as *fully qualified* labels: the programmer wrote the full enum class name.
However there are some situations where Hack can infer the class name; for example,
the previous calls could be written as `full_print(#A)` and `full_print(#B)`, leaving `E` implicit.
This is only allowed when there is enough type information to infer the right enum class name. For example, `$x = #A` is not allowed and will result in a type error.

## Equality testing

Enum class labels can be tested for equality using the `===` operator or a switch statement:

```hack file:label.hack
function test_eq(\HH\EnumClass\Label<E, int> $label): void {
  if ($label === E#A) { echo "label is A\n"; }
  switch ($label) {
    case E#A: break;
    case E#B: break;
  }
}
```

Because the runtime doesn’t have all the typing information available, these tests only check the name component of a label. It means that for any two enum classes `E` and `F`, `E#A === F#A` will be true despite being from different enum classes. Also the support type (int in the case of the enum class `E`) is not taken into account either.

```hack
class Foo {}

enum class F: Foo {
  Foo A = new Foo();
}

// E#A === E#B is false
// E#A === F#A is true
```

## Enum class labels and abstract enum classes
Abstract enum classes support labels like any other enum class. The main difference is that an abstract enum class only provides the `nameOf` static method.
Since some of its members may be abstracted away, abstract enum classes do no provide the `valueOf()` or `getValues()` static methods.

## Known corner cases

### The `#` character is no longer a single-line comment
This feature relies on the fact that Hack and HHVM no longer consider the character `#` as a single-line comment. Please use `//` for such purpose.

### Labels and values cannot be exchanged
If a method is expecting a label, one cannot pass in a value, and vice versa: `full_print(E::A)` will result in a type error and so will `partial_print(E#A)`.

### `MemberOf` is covariant, `Label` is invariant

Let’s recall the definition of `HH\MemberOf` and `HH\EnumClass\Label` along with some basic definitions:

```hack
newtype MemberOf<-TEnumClass, +TType> as TType = TType;
newtype Label<-TEnumClass, TType> = mixed;

class A {}
class B extends A {}
enum class G: A {
  A X = new A();
  B Y = new B();
}
```

Firstly, `HH\MemberOf` has a `as TType` constraint. It means that since `G::X` is of type `HH\MemberOf<G, A>`, it is also of type `A`. For the same reasons, `G::Y` is of type `HH\MemberOf<G, B>` and `B`.
Secondly, `HH\MemberOf` is covariant in `TType`. Since `B extends A`, it means that `G::Y` is also of type `HH\MemberOf<G, A>`. And because of all of that, `G::Y` is also of type `A`.

Enum class values behave just like the underlying data they are set to.

On the other hand, `HH\EnumClass\Label` is invariant in `TType`. It means that while `G#Y` is of type `HH\EnumClass\Label<G, B>`, it is not of type `HH\EnumClass\Label<G, A>`. Labels are opaque handles to access data; you can think about them as maps from names to types. Their typing has to be more strict, especially if we want to be able to extend this concept to other parts of a class (reflection like access to methods, properties, …). To make sure these possible extensions remain possible, we enforce a stricter typing for labels than for values.
