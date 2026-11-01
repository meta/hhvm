<?hh

// The class's own constructor replaces OtherTrait's, so the property MyTrait
// adds through OtherTrait is never initialized.
trait MyTrait {
  private int $limit;
}

trait OtherTrait {
  use MyTrait;

  public function __construct() {
    $this->limit = 2;
  }
}

class MyClassUsesMyTrait {
  use OtherTrait;
  public function __construct() {}

  // Fatal: Expected int, got null.
  public function getLimit(): int {
    return $this->limit;
  }
}

<<__EntryPoint>>
function my_main(): void {
  (new MyClassUsesMyTrait())->getLimit();
}
