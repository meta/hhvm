<?hh

final class Box<+T> {
  public function __construct(private T $t, private vec<T> $all = vec[]) {}
  public function get(): T {
    return $this->t;
  }

  // $box may be a Box<int> passed as a Box<arraykey>.
  public static function overwrite(Box<arraykey> $box, arraykey $in): void {
    $box->t = $in;
  }
  public static function append(Box<arraykey> $box, arraykey $in): void {
    $box->all[] = $in;
  }
  public static function call_set(Box<arraykey> $box, arraykey $in): void {
    $box->set($in);
  }

  // Through $this, the type arguments are exact.
  private function reset(T $t): void {
    $this->t = $t;
    $this->set($t);
  }
  // Reading a covariant member through another receiver is fine.
  private function same(Box<T> $other): bool {
    return $other->t === $this->t && $other->peek() === $this->t;
  }
  private function set(T $t): void {
    $this->t = $t;
  }
  private function peek(): T {
    return $this->t;
  }
}

final class Sink<-T> {
  public function __construct(private ?T $last = null) {}
  // $sink may be a Sink<mixed> passed as a Sink<int>.
  public static function last(Sink<int> $sink): ?int {
    return $sink->lastValue();
  }
  private function lastValue(): ?T {
    return $this->last;
  }
}

final class Invariant<T> {
  public function __construct(private T $t) {}
  public static function overwrite(Invariant<int> $i, int $in): void {
    $i->t = $in;
  }
}

final class ContraBox<-T> {
  public function sink(T $_): void {}
}

final class Wobble<+T> {
  public function __construct(private ContraBox<T> $contra, private T $in) {}

  // ContraBox<T> uses T contravariantly, so writing it through another
  // receiver is fine.
  public static function overwrite(
    Wobble<arraykey> $that,
    ContraBox<arraykey> $in,
  ): void {
    $that->contra = $in;
  }
  public function apply(): void {
    $this->contra->sink($this->in);
  }

  // Calling a method whose return type uses T contravariantly is not: an
  // int-only sink would be returned as a ContraBox<arraykey>.
  public static function leak(Wobble<arraykey> $that): ContraBox<arraykey> {
    return $that->contra();
  }
  private function contra(): ContraBox<T> {
    return $this->contra;
  }

  // A callback parameter uses T covariantly in the method type.
  public static function visit(
    Wobble<arraykey> $that,
    (function(arraykey): void) $f,
  ): void {
    $that->each($f);
  }
  private function each((function(T): void) $f): void {
    $f($this->in);
  }
}

final class Handler<-T> {
  public function __construct(
    private (function(T): void) $on,
    private ?T $last = null,
  ) {}

  // $h may be a Handler<mixed> passed as a Handler<int>: an int-only callback
  // would be stored where any value can be passed to it.
  public static function replace(Handler<int> $h, (function(int): void) $on): void {
    $h->on = $on;
  }
  // ?T uses T covariantly, so this write is fine.
  public static function remember(Handler<int> $h, int $x): void {
    $h->last = $x;
  }
}
