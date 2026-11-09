<?hh

function register(mixed $_): void {}

final class FinalPassesThis {
  private int $n;
  public function __construct() {
    $this->n = 1;
    register($this);
    $this->hook();
  }
  public function hook(): void {}
}

class CallsPrivate {
  private int $n;
  public function __construct() {
    $this->init();
  }
  private function init(): void {
    $this->n = 1;
  }
}

class UsesProperties {
  private int $n;
  private vec<int> $v;
  public function __construct() {
    $this->n = 1;
    $this->v = vec[$this->n];
    register($this->v);
  }
}

class CallsStatic {
  public function __construct() {
    static::make();
    self::make();
  }
  public static function make(): void {}
}

class CallsParentConstructor extends CallsStatic {
  public function __construct() {
    parent::__construct();
  }
}

final class FinalCallsParentMethod extends CallsStatic {
  private int $n;
  public function __construct() {
    parent::__construct();
    $this->n = 1;
    parent::make();
  }
}

// A subclass can't replace a final constructor, so it has no constructor of
// its own that could run this one before initializing its properties.
class FinalConstructorCallsHook {
  final public function __construct() {
    $this->hook();
  }
  public function hook(): void {}
}

trait TraitFinalConstructorCallsHook {
  final public function __construct() {
    $this->hook();
  }
  public function hook(): void {}
}
