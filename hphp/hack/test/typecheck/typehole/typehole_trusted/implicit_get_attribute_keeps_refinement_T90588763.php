<?hh

class :xhp {
  attribute string attr;
  public mixed $prop;

  public function __construct(
    darray<string,mixed> $_,
    varray<mixed> $_,
    string $_,
    int $_,
  ) {
    $this->prop = 42;
  }

  public function getAttribute(string $_, mixed $_ = null): dynamic {
    $this->prop = "";
    return "";
  }
}

function takes_int(int $_): void {}

<<__EntryPoint>>
function oops(): void {
  $xhp = <xhp />; // $xhp->prop holds an int due to constructor
  if ($xhp->prop is int) {
    $xhp->:attr; // $xhp->prop holds a string due to call to getAttribute
    takes_int($xhp->prop); // We allow string to be passed into an int parameter
  }
}
