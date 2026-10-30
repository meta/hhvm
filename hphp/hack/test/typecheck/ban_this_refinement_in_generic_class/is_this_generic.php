<?hh

// `is this` refines $other to this box's own T, but the runtime only checks
// the class.
class Box<T> {
  public function __construct(private T $t) {}
  public function get(): T {
    return $this->t;
  }
  public function adopt(mixed $other): void {
    if ($other is this) {
      $this->t = $other->t;
    }
  }
  public function adopt_nullable(mixed $other): ?this {
    return $other as ?this;
  }
  // Wildcards refine to fresh type arguments, which is sound.
  public function adopt_wildcard(mixed $other): void {
    if ($other is Box<_>) {
      $other->get();
    }
  }
}

abstract class WithConst<T> {
  <<__Enforceable>>
  abstract const type TC;
  // A type constant of `this` is not a refinement to `this`.
  public function tc(mixed $m): ?this::TC {
    return $m as ?this::TC;
  }
}

// A final class without type parameters is exactly `this`.
final class Plain {
  public function same(mixed $m): ?this {
    if ($m is this) {
      return $m;
    }
    return null;
  }
}
