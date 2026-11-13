# Trait & Interface Requirements

Trait and interface requirements allow you to restrict the use of these constructs by specifying what classes may actually use a trait or
implement an interface. This can simplify long lists of `abstract` method requirements, and provide a hint to the reader as to the
intended use of the trait or interface.

## Syntax

To introduce a trait requirement, you can have one or more of the following in your trait:

```hack no-extract
require extends <class name>;
require class <class name>;
require implements <interface name>;
```

To introduce an interface requirement, you can have one or more of following in your interface:

```hack no-extract
require extends <class name>;
```

## Traits

Here is an example of a trait that introduces a class and interface requirement, and shows a class that meets the requirement:

```hack
abstract class Machine {
  public function openDoors(): void {
    return;
  }
  public function closeDoors(): void {
    return;
  }
}
interface Fliers {
  public function fly(): bool;
}

trait Plane {
  require extends Machine;
  require implements Fliers;

  public function takeOff(): bool {
    $this->openDoors();
    $this->closeDoors();
    return $this->fly();
  }
}

class AirBus extends Machine implements Fliers {
  use Plane;

  public function fly(): bool {
    return true;
  }
}

<<__EntryPoint>>
function run(): void {
  $ab = new AirBus();
  \var_dump($ab);
  \var_dump($ab->takeOff());
}
```

Here is an example of a trait that introduces a class and interface requirement, and shows a class that *does not* meet the requirement:

```hack error
abstract class Machine {
  public function openDoors(): void {
    return;
  }
  public function closeDoors(): void {
    return;
  }
}
interface Fliers {
  public function fly(): bool;
}

trait Plane {
  require extends Machine;
  require implements Fliers;

  public function takeOff(): bool {
    $this->openDoors();
    $this->closeDoors();
    return $this->fly();
  }
}

// Having this will not only cause a typechecker error, but also cause a fatal
// error in HHVM since we did not meet the trait requirement.
class Paper implements Fliers {
  use Plane;

  public function fly(): bool {
    return false;
  }
}

<<__EntryPoint>>
function run(): void {
  // This code will not run in HHVM because of the problem mentioned above.
  $p = new Paper();
  \var_dump($p);
  \var_dump($p->takeOff());
}
```

The `require extends` constraints should be taken literally: the class that uses the trait *must* be a **strict** subclass of that in the `require extends` constraint.

A `require class <class name>;` constraint in a trait specifies instead that the trait can only be used by the named, non-generic class.  The required class may be abstract, final, or non-final.  The required class must use the trait, either directly or transitively through another trait. Using the trait only in a subclass of the named class is an error.  The typical use of `require class` constraints is to split the implementation of a class into a class and one or more traits:

```hack
trait T {
  require class C;

  public function foo(): void {
    $this->bar();
  }
}

class C {
  use T;

  private function bar(): void {}
}

class D extends C {} // Inherits T's members from C.
```

When the required class is final, the typechecker can assume that `this` is exactly that class while checking the trait. When the required class is non-final, a trait method may execute on an instance of a subclass, so the typechecker assumes only that `this` is a subtype of the required class. For example, a trait requiring a non-final class `C` cannot return `new C()` from a function whose return type is `this`.

*Note*: the `require this as <class name>;` constraint allowed the trait to be used by any non-generic class which is a subtype of `<class name>`, including `<class name>` itself.  This trait constraint will be **deprecated** and all instances can be replaced by `require class`.

## Interfaces

Here is an example of an interface that introduces a class requirement, and shows a class that meets the requirement:

```hack
abstract class Machine {
  public function openDoors(): void {
    return;
  }
  public function closeDoors(): void {
    return;
  }
}
interface Fliers {
  require extends Machine;
  public function fly(): bool;
}

class AirBus extends Machine implements Fliers {
  public function takeOff(): bool {
    $this->openDoors();
    $this->closeDoors();
    return $this->fly();
  }

  public function fly(): bool {
    return true;
  }
}

<<__EntryPoint>>
function run(): void {
  $ab = new AirBus();
  \var_dump($ab);
  \var_dump($ab->takeOff());
}
```

Here is an example of an interface that introduces a class requirement, and shows a class that *does not* meet the requirement:

```hack error
abstract class Machine {
  public function openDoors(): void {
    return;
  }
  public function closeDoors(): void {
    return;
  }
}
interface Fliers {
  require extends Machine;
  public function fly(): bool;
}

// Having this will not only cause a typechecker error, but also cause a fatal
// error in HHVM since we did not meet the interface requirement (extending
// Machine).
class Paper implements Fliers {
  public function fly(): bool {
    return false;
  }
}

<<__EntryPoint>>
function run(): void {
  // This code will actually not run in HHVM because of the fatal mentioned
  // above.
  $p = new Paper();
  \var_dump($p);
  \var_dump($p->takeOff());
}
```

**NOTE**: trait cannot be used as a type, comparing to some other languages. Only class and interface are types. For example,

```hack no-extract
trait T {}
class C { use T; }
$a = new C();
$j = ($a is C);
$k = ($a is T); # error!
```
leads to error

```
Hit fatal : "is" and "as" operators cannot be used with a trait
    #0 at [:1]
    #1 include(), called at [:1]
    #2 include(), called at [:0]
Hit fatal : "is" and "as" operators cannot be used with a trait
Failed to evaluate expression
```
