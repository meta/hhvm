<?hh
<<file:__EnableUnstableFeatures('require_constraint')>>

class WWWTest {}

class Target {
  <<__TestsBypassVisibility>>
  private function secret(): void {}
}

trait RequireExtendsTestContext {
  require extends WWWTest;

  public function test_require_extends(Target $obj): void {
    $obj->secret(); // ok: require extends makes this a test context
  }
}

/* HH_FIXME[4466] intentionally used by a subclass of WWWTest */
trait RequireThisAsTestContext {
  /* HH_IGNORE[12043] require-this-as constraint; class does not use trait */
  require this as WWWTest;

  public function test_require_this_as(Target $obj): void {
    $obj->secret(); // ok: require this as makes this a test context
  }
}

trait RequireClassTestContext {
  require class ExactWWWTest;

  public function test_require_class(Target $obj): void {
    $obj->secret(); // ok: require class implies a class extending the test base
  }
}

class TestWithRequirements extends WWWTest {
  use RequireExtendsTestContext;
  use RequireThisAsTestContext;
}

final class ExactWWWTest extends WWWTest {
  use RequireClassTestContext;
}
