<?hh

function register(mixed $_): void {}

class PassesThis {
  public function __construct() {
    register($this);
  }
}

class CallsOverridable {
  public function __construct() {
    $this->hook();
  }
  public function hook(): void {}
}

class CallsFinal {
  public function __construct() {
    $this->fixed();
  }
  final public function fixed(): void {}
}

abstract class TemplateMethod {
  public function __construct() {
    $this->init();
  }
  abstract protected function init(): void;
}

trait TraitCallsOverridable {
  public function __construct() {
    $this->hook();
  }
  public function hook(): void {}
}

class EscapesAfterInit {
  private int $n;
  public function __construct() {
    $this->n = 1;
    register($this);
  }
}
